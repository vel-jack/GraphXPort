#include "GraphXPortMenu.h"

#include "BlueprintEditor.h"
#include "DesktopPlatformModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphXPortBeautifier.h"
#include "GraphXPortSerializer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDesktopPlatform.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GraphXPortMenu"

namespace
{
    void Notify(const FText& Text, const SNotificationItem::ECompletionState State)
    {
        FNotificationInfo Info(Text);
        Info.ExpireDuration = 4.0f;
        Info.bUseSuccessFailIcons = true;
        const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
        if (Item.IsValid()) Item->SetCompletionState(State);
    }

    TArray<const UEdGraphNode*> ResolveSelection(
        const UBlueprint* Blueprint,
        const UEdGraph* Graph,
        const UEdGraphNode* ClickedNode)
    {
        TArray<const UEdGraphNode*> Result;
        if (Blueprint != nullptr)
        {
            // FKismetEditorUtilities::GetIBlueprintEditorForObject is the engine's own
            // blessed lookup for "the editor currently open on this Blueprint" - it replaces
            // the previous AssetEditorSubsystem::FindEditorForAsset() call and its unchecked
            // static_cast<const FBlueprintEditor*> gated by a GetEditorName() == "BlueprintEditor"
            // string comparison, which silently produced an empty selection for any editor
            // other than the exact base Blueprint editor (e.g. Widget/Anim Blueprint editors).
            // IBlueprintEditor itself intentionally does not expose the full node selection
            // set (only AddToSelection/GetNumberOfSelectedNodes), and every current
            // implementer of it for a Blueprint asset derives from FBlueprintEditor, so the
            // cast below is type-safe in practice even though the interface has no RTTI.
            const TSharedPtr<IBlueprintEditor> Editor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false);
            if (Editor.IsValid())
            {
                const TSharedPtr<FBlueprintEditor> ConcreteEditor = StaticCastSharedPtr<FBlueprintEditor>(Editor);
                const FGraphPanelSelectionSet SelectedObjects = ConcreteEditor->GetSelectedNodes();
                for (UObject* SelectedObject : SelectedObjects)
                {
                    const UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject);
                    if (SelectedNode != nullptr && SelectedNode->GetGraph() == Graph)
                    {
                        Result.Add(SelectedNode);
                    }
                }
            }
        }

        if (ClickedNode != nullptr && !Result.Contains(ClickedNode))
        {
            Result.Reset();
            Result.Add(ClickedNode);
        }
        return Result;
    }

    FString SafeFileBase(const FGraphXPortDocument& Document)
    {
        return FPaths::MakeValidFileName(Document.BlueprintName + TEXT("_") + Document.GraphName + TEXT("_") + Document.Scope);
    }

    bool NodeHasExecPin(const UEdGraphNode* Node)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
            if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return true;
        return false;
    }

    // A single selected node can't be "arranged" against nothing, so Beautify treats that
    // as "format everything downstream of this node" instead of requiring a manual
    // multi-select: walk forward through the node's own execution chain (every branch,
    // not just one path), and at every node visited also walk backward through its data
    // input pins so pure/data sources feeding the chain come along too. This never leaves
    // the node's own downstream chain and never crosses into an unrelated part of the
    // graph a data output happens to also feed.
    TArray<UEdGraphNode*> ExpandFromSingleNode(UEdGraphNode* Start)
    {
        TArray<UEdGraphNode*> Result;
        TSet<UEdGraphNode*> Visited{Start};
        TArray<UEdGraphNode*> Queue{Start};
        while (!Queue.IsEmpty())
        {
            UEdGraphNode* Node = Queue.Pop();
            Result.Add(Node);
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr) continue;
                const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                const bool bFollow = (bExec && Pin->Direction == EGPD_Output) || (!bExec && Pin->Direction == EGPD_Input);
                if (!bFollow) continue;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Other = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                    if (Other != nullptr && !Visited.Contains(Other))
                    {
                        Visited.Add(Other);
                        Queue.Add(Other);
                    }
                }
            }
        }
        return Result;
    }
}

void FGraphXPortMenu::AddContextSection(UToolMenu* Menu)
{
    if (Menu == nullptr) return;
    const UGraphNodeContextMenuContext* Context = Menu->FindContext<UGraphNodeContextMenuContext>();
    if (Context == nullptr || Context->Blueprint == nullptr || Context->Graph == nullptr || Context->Node == nullptr
        || !Context->Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        return;
    }

    FToolMenuSection& Section = Menu->AddSection(TEXT("GraphXPort"));
    Section.AddSubMenu(
        TEXT("GraphXPortSubmenu"),
        LOCTEXT("SubmenuLabel", "GraphXPort"),
        LOCTEXT("SubmenuTooltip", "Copy or export this Blueprint graph flow in portable formats."),
        FNewToolMenuDelegate::CreateStatic(&FGraphXPortMenu::PopulateSubmenu),
        false,
        FSlateIcon());
}

void FGraphXPortMenu::PopulateSubmenu(UToolMenu* Menu)
{
    if (Menu == nullptr) return;
    const UGraphNodeContextMenuContext* Context = Menu->FindContext<UGraphNodeContextMenuContext>();
    if (Context == nullptr || Context->Blueprint == nullptr || Context->Graph == nullptr || Context->Node == nullptr) return;

    const TWeakObjectPtr<const UBlueprint> Blueprint(Context->Blueprint);
    const TWeakObjectPtr<const UEdGraph> Graph(Context->Graph);
    const TWeakObjectPtr<const UEdGraphNode> Node(Context->Node);
    FToolMenuSection& Section = Menu->AddSection(TEXT("GraphXPortActions"));

    const auto AddAction = [&](const FName Name, const FText& Label, const FText& Tooltip,
        const EGraphXPortFormat Format, const bool bEntireGraph, const bool bSaveFile)
    {
        Section.AddMenuEntry(Name, Label, Tooltip, FSlateIcon(), FUIAction(FExecuteAction::CreateLambda(
            [Blueprint, Graph, Node, Format, bEntireGraph, bSaveFile]()
            {
                Export(Blueprint.Get(), Graph.Get(), Node.Get(), Format, bEntireGraph, bSaveFile);
            })));
    };

    AddAction(TEXT("CopyAiText"), LOCTEXT("CopyAiText", "Copy Selection for AI"),
        LOCTEXT("CopyAiTextTip", "Copy compact Markdown describing the selected nodes and their connections."),
        EGraphXPortFormat::AiText, false, false);
    AddAction(TEXT("CopyAscii"), LOCTEXT("CopyAscii", "Copy Selection as ASCII Flow"),
        LOCTEXT("CopyAsciiTip", "Copy a concise text diagram of the selected flow."),
        EGraphXPortFormat::Ascii, false, false);
    AddAction(TEXT("CopyMermaid"), LOCTEXT("CopyMermaid", "Copy Selection as Mermaid"),
        LOCTEXT("CopyMermaidTip", "Copy a Mermaid flowchart generated from the selected nodes."),
        EGraphXPortFormat::Mermaid, false, false);
    Section.AddMenuEntry(TEXT("BeautifySelection"),
        LOCTEXT("BeautifySelection", "Beautify Selection"),
        LOCTEXT("BeautifySelectionTip", "Rearrange the selected nodes left-to-right by execution flow, anchored on the node you right-clicked. Select one node to format its whole downstream chain, or select several to arrange just those. Undoable; does not change pins or connections."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateLambda(
            [Blueprint, Graph, Node]() { Beautify(Blueprint.Get(), Graph.Get(), Node.Get()); })));

    Section.AddSeparator(TEXT("GraphScopeSeparator"));
    AddAction(TEXT("CopyGraphAiText"), LOCTEXT("CopyGraphAiText", "Copy Entire Graph for AI"),
        LOCTEXT("CopyGraphAiTextTip", "Copy every node and connection in the current function, event, macro, or graph."),
        EGraphXPortFormat::AiText, true, false);
    AddAction(TEXT("ExportGraphJson"), LOCTEXT("ExportGraphJson", "Export Entire Graph to File..."),
        LOCTEXT("ExportGraphJsonTip", "Save the complete current graph as a .graphx.json file."),
        EGraphXPortFormat::Json, true, true);
}

void FGraphXPortMenu::Export(
    const UBlueprint* Blueprint,
    const UEdGraph* Graph,
    const UEdGraphNode* ClickedNode,
    const EGraphXPortFormat Format,
    const bool bEntireGraph,
    const bool bSaveFile)
{
    if (Blueprint == nullptr || Graph == nullptr)
    {
        Notify(LOCTEXT("ContextExpired", "GraphXPort export context is no longer available."), SNotificationItem::CS_Fail);
        return;
    }

    FGraphXPortDocument Document;
    FString Error;
    const bool bBuilt = bEntireGraph
        ? FGraphXPortSerializer::BuildGraph(Blueprint, Graph, Document, Error)
        : FGraphXPortSerializer::BuildSelection(Blueprint, Graph, ResolveSelection(Blueprint, Graph, ClickedNode), Document, Error);
    if (!bBuilt)
    {
        Notify(FText::FromString(TEXT("GraphXPort: ") + Error), SNotificationItem::CS_Fail);
        return;
    }

    const FString Output = FGraphXPortFormatter::Format(Document, Format);
    if (!bSaveFile)
    {
        FPlatformApplicationMisc::ClipboardCopy(*Output);
        Notify(FText::Format(LOCTEXT("Copied", "GraphXPort copied {0} nodes and {1} connections."),
            FText::AsNumber(Document.Nodes.Num()), FText::AsNumber(Document.Edges.Num())), SNotificationItem::CS_Success);
        return;
    }

    const FString DefaultDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GraphXPort"));
    IFileManager::Get().MakeDirectory(*DefaultDirectory, true);
    const FString DefaultFilename = SafeFileBase(Document) + TEXT(".") + FGraphXPortFormatter::Extension(Format);
    TArray<FString> OutputFiles;
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    if (DesktopPlatform == nullptr || !DesktopPlatform->SaveFileDialog(
        ParentWindow,
        TEXT("Export GraphXPort Flow"),
        DefaultDirectory,
        DefaultFilename,
        FGraphXPortFormatter::FileFilter(Format),
        EFileDialogFlags::None,
        OutputFiles) || OutputFiles.IsEmpty())
    {
        return;
    }

    if (!FFileHelper::SaveStringToFile(Output, *OutputFiles[0], FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Notify(LOCTEXT("SaveFailed", "GraphXPort could not write the selected file."), SNotificationItem::CS_Fail);
        return;
    }
    Notify(FText::FromString(TEXT("GraphXPort exported: ") + OutputFiles[0]), SNotificationItem::CS_Success);
}

void FGraphXPortMenu::Beautify(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* ClickedNode)
{
    if (Blueprint == nullptr || Graph == nullptr)
    {
        Notify(LOCTEXT("ContextExpired", "GraphXPort export context is no longer available."), SNotificationItem::CS_Fail);
        return;
    }

    TArray<UEdGraphNode*> SelectedNodes;
    for (const UEdGraphNode* Node : ResolveSelection(Blueprint, Graph, ClickedNode))
    {
        SelectedNodes.Add(const_cast<UEdGraphNode*>(Node));
    }

    // Two supported workflows share this one menu entry: a manual multi-select is
    // beautified exactly as selected (nothing outside it is touched, and the layout never
    // follows a connection past the last selected node); selecting a single node instead
    // expands to that node's own downstream execution chain so "format everything from
    // here to the end" doesn't require manually selecting the whole chain first.
    if (SelectedNodes.Num() == 1 && NodeHasExecPin(SelectedNodes[0]))
    {
        SelectedNodes = ExpandFromSingleNode(SelectedNodes[0]);
    }

    // The clicked node anchors the whole layout at its own current position - it is the
    // "first node" everything else is arranged relative to, whether that's the one node
    // the user selected (single-node case) or whichever node they right-clicked within a
    // larger multi-selection.
    UEdGraphNode* PreferredAnchor = const_cast<UEdGraphNode*>(ClickedNode);

    TMap<UEdGraphNode*, FVector2D> Positions;
    if (SelectedNodes.Num() < 2 || !FGraphXPortBeautifier::ComputeLayout(SelectedNodes, Positions, PreferredAnchor))
    {
        Notify(LOCTEXT("BeautifyNeedsTwo", "GraphXPort: select at least two nodes to beautify."), SNotificationItem::CS_Fail);
        return;
    }

    TArray<TPair<UEdGraphNode*, FIntPoint>> Changes;
    for (const TPair<UEdGraphNode*, FVector2D>& Pair : Positions)
    {
        UEdGraphNode* Node = Pair.Key;
        const int32 NewX = FMath::RoundToInt(Pair.Value.X);
        const int32 NewY = FMath::RoundToInt(Pair.Value.Y);
        if (Node->NodePosX != NewX || Node->NodePosY != NewY)
        {
            Changes.Add({Node, FIntPoint(NewX, NewY)});
        }
    }
    if (Changes.IsEmpty())
    {
        Notify(LOCTEXT("BeautifyNoChange", "GraphXPort: selection is already arranged."), SNotificationItem::CS_Success);
        return;
    }

    // Beautify is the plugin's one intentional exception to "export never mutates": it is
    // a cosmetic, position-only edit to the user's own selection, so it follows normal
    // Unreal editor edit conventions (transaction + Modify() + dirty) rather than the
    // export path's read-only guarantee.
    const FScopedTransaction Transaction(LOCTEXT("BeautifyTransaction", "Beautify GraphXPort Selection"));
    for (const TPair<UEdGraphNode*, FIntPoint>& Change : Changes)
    {
        Change.Key->Modify();
        Change.Key->NodePosX = Change.Value.X;
        Change.Key->NodePosY = Change.Value.Y;
    }
    const_cast<UBlueprint*>(Blueprint)->MarkPackageDirty();
    Notify(FText::Format(LOCTEXT("Beautified", "GraphXPort rearranged {0} node(s)."), FText::AsNumber(Changes.Num())),
        SNotificationItem::CS_Success);
}

#undef LOCTEXT_NAMESPACE
