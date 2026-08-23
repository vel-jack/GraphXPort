#include "GraphXPortMenu.h"

#include "BlueprintEditor.h"
#include "DesktopPlatformModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphXPortSerializer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDesktopPlatform.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
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
        if (Blueprint != nullptr && GEditor != nullptr)
        {
            UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            IAssetEditorInstance* EditorInstance = AssetEditors != nullptr
                ? AssetEditors->FindEditorForAsset(const_cast<UBlueprint*>(Blueprint), false)
                : nullptr;
            if (EditorInstance != nullptr && EditorInstance->GetEditorName() == TEXT("BlueprintEditor"))
            {
                const FBlueprintEditor* BlueprintEditor = static_cast<const FBlueprintEditor*>(EditorInstance);
                const FGraphPanelSelectionSet SelectedObjects = BlueprintEditor->GetSelectedNodes();
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

#undef LOCTEXT_NAMESPACE
