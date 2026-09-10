#include "SGraphXPortDiffPanel.h"

#include "DiffUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphDiffControl.h"
#include "GraphEditor.h"
#include "GraphXPortGitBlob.h"
#include "GraphXPortTempPackage.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailsView.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

namespace GraphXPortDiffPanelPrivate
{
    static const FLinearColor CategoryColor(0.85f, 0.85f, 0.85f);
    static const FLinearColor AddedColor(0.3f, 0.85f, 0.35f);
    static const FLinearColor RemovedColor(0.95f, 0.35f, 0.3f);
    static const FLinearColor ChangedColor(0.9f, 0.75f, 0.25f);
    static const FLinearColor UnchangedColor(0.75f, 0.75f, 0.75f);
    static const FLinearColor ConflictColor(1.f, 0.42f, 0.15f);

    static TMap<FName, USCS_Node*> GatherComponentNodes(const UBlueprint* Blueprint)
    {
        TMap<FName, USCS_Node*> Nodes;
        if (Blueprint && Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node)
                {
                    Nodes.Add(Node->GetVariableName(), Node);
                }
            }
        }
        return Nodes;
    }

    static FText DescribeVariableType(const FBPVariableDescription& Variable)
    {
        FString TypeText = Variable.VarType.PinCategory.ToString();
        if (const UObject* SubCategory = Variable.VarType.PinSubCategoryObject.Get())
        {
            TypeText += TEXT(" ") + SubCategory->GetName();
        }
        return FText::FromString(TypeText);
    }
}

void SGraphXPortDiffPanel::Construct(const FArguments& InArgs)
{
    OnFileResolved = InArgs._OnFileResolved;

    DiffTreeWidget = BuildDiffTreeWidget();

    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            BuildToolbarRow()
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(PanesContainer, SBox)
        ]
    ];

    RebuildPanes();
}

SGraphXPortDiffPanel::~SGraphXPortDiffPanel()
{
    ReleaseLoadedRevisions();
}

TSharedRef<SWidget> SGraphXPortDiffPanel::GetDifferencesTree() const
{
    return DiffTreeWidget.ToSharedRef();
}

void SGraphXPortDiffPanel::AddReferencedObjects(FReferenceCollector& Collector)
{
    for (FSideData* Side : { &BaseSide, &MineSide, &TheirsSide, &ResultSide })
    {
        Collector.AddReferencedObject(Side->Asset);
        Collector.AddReferencedObject(Side->Blueprint);
        Collector.AddReferencedObject(Side->Package);
    }
}

void SGraphXPortDiffPanel::SetFile(const FString& InRepoRoot, TSharedPtr<FGraphXPortGitFileEntry> InEntry)
{
    ReleaseLoadedRevisions();

    RepoRoot = InRepoRoot;
    Entry = InEntry;

    RebuildSides();
    CollectGraphNames();
    ComputeAllDiffs();
    BuildDiffTree();
    ApplySelectedGraphDiffResults();
    RebuildPanes();

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::GetEmpty());
    }
}

void SGraphXPortDiffPanel::ReleaseLoadedRevisions()
{
    // Drop the SGraphEditor panes first: they point straight at graphs owned by these
    // revisions, and must not outlive our references to them.
    if (PanesContainer.IsValid())
    {
        PanesContainer->SetContent(SNullWidget::NullWidget);
    }
    LeftPane.Reset();
    ResultPane.Reset();
    TheirsPane.Reset();
    LeftPaneCommands.Reset();
    ResultPaneCommands.Reset();
    TheirsPaneCommands.Reset();

    // Diff results and tree rows hold raw node pointers into those revisions.
    GraphDiffs.Reset();
    GraphNames.Reset();
    SelectedGraphName = NAME_None;
    bShowMembers = false;
    DiffTreeRoots.Reset();
    if (DiffTreeView.IsValid())
    {
        DiffTreeView->RequestTreeRefresh();
    }

    // Diff-only revisions are left for GC rather than force-unloaded; dropping these
    // references is what makes them collectable.
    BaseSide = FSideData();
    MineSide = FSideData();
    TheirsSide = FSideData();
    ResultSide = FSideData();
    bLeftShowsMine = false;
}

void SGraphXPortDiffPanel::LoadTempSide(FSideData& Side, const FString& RefSpec)
{
    FGraphXPortLoadedAsset Loaded;
    FString Error;
    if (FGraphXPortTempPackage::LoadBlobAsAsset(RepoRoot, RefSpec, Entry->RepoRelativePath, Loaded, Error))
    {
        Side.Asset = Loaded.Asset;
        Side.Blueprint = Loaded.Blueprint;
        Side.Package = Loaded.Package;
        Side.Kind = Loaded.Kind;
        Side.bIsTemp = true;
        Side.UnavailableReason = Error;
        FGraphXPortAssetDiff::GetEntries(Loaded.Asset, Side.Entries);
        FGraphXPortAssetDiff::GetColumns(Loaded.Asset, Side.Columns);
    }
    else
    {
        Side.UnavailableReason = Error;
    }
}

void SGraphXPortDiffPanel::LoadRealSide(FSideData& Side, bool bEditable)
{
    FGraphXPortLoadedAsset Loaded;
    FString Error;
    if (FGraphXPortTempPackage::ResolveRealAsset(RepoRoot, Entry->RepoRelativePath, Loaded, Error))
    {
        Side.Asset = Loaded.Asset;
        Side.Blueprint = Loaded.Blueprint;
        Side.Package = Loaded.Package;
        Side.Kind = Loaded.Kind;
        Side.bEditable = bEditable;
        Side.UnavailableReason = Error;
        FGraphXPortAssetDiff::GetEntries(Loaded.Asset, Side.Entries);
        FGraphXPortAssetDiff::GetColumns(Loaded.Asset, Side.Columns);
    }
    else
    {
        Side.UnavailableReason = Error;
    }
}

void SGraphXPortDiffPanel::RebuildSides()
{
    BaseSide = FSideData();
    MineSide = FSideData();
    TheirsSide = FSideData();
    ResultSide = FSideData();

    if (!Entry.IsValid())
    {
        Mode = EGraphXPortDiffMode::None;
        return;
    }

    switch (Entry->StatusCode)
    {
    case TEXT('U'):
        Mode = EGraphXPortDiffMode::Conflict3Way;
        BaseSide.Label = LOCTEXT("Base", "Base (common ancestor)");
        LoadTempSide(BaseSide, TEXT(":1"));
        MineSide.Label = LOCTEXT("Mine", "Mine (yours)");
        LoadTempSide(MineSide, TEXT(":2"));
        TheirsSide.Label = LOCTEXT("Theirs", "Theirs (incoming)");
        LoadTempSide(TheirsSide, TEXT(":3"));
        ResultSide.Label = LOCTEXT("Result", "Result (editable)");
        LoadRealSide(ResultSide, /*bEditable=*/true);
        break;

    case TEXT('M'):
    case TEXT('R'):
        Mode = EGraphXPortDiffMode::ModifiedCompare;
        BaseSide.Label = LOCTEXT("Head", "HEAD");
        LoadTempSide(BaseSide, TEXT("HEAD"));
        ResultSide.Label = LOCTEXT("Working", "Working tree (editable)");
        LoadRealSide(ResultSide, /*bEditable=*/true);
        break;

    case TEXT('D'):
        Mode = EGraphXPortDiffMode::SingleReadOnly;
        BaseSide.Label = LOCTEXT("HeadDeleted", "HEAD (deleted in working tree)");
        LoadTempSide(BaseSide, TEXT("HEAD"));
        break;

    case TEXT('A'):
    case TEXT('?'):
    default:
        Mode = EGraphXPortDiffMode::NewFile;
        ResultSide.Label = LOCTEXT("New", "New file (editable)");
        LoadRealSide(ResultSide, /*bEditable=*/true);
        break;
    }
}

void SGraphXPortDiffPanel::CollectGraphNames()
{
    TSet<FName> Names;
    for (const FSideData* Side : { &BaseSide, &MineSide, &TheirsSide, &ResultSide })
    {
        if (Side->Blueprint)
        {
            TArray<UEdGraph*> Graphs;
            Side->Blueprint->GetAllGraphs(Graphs);
            for (const UEdGraph* Graph : Graphs)
            {
                if (Graph)
                {
                    Names.Add(Graph->GetFName());
                }
            }
        }
    }

    GraphNames = Names.Array();
    GraphNames.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });

    SelectedGraphName = NAME_None;
    for (const FName& Name : GraphNames)
    {
        if (Name == FName(TEXT("EventGraph")))
        {
            SelectedGraphName = Name;
            break;
        }
    }
    if (SelectedGraphName.IsNone() && GraphNames.Num() > 0)
    {
        SelectedGraphName = GraphNames[0];
    }
}

UEdGraph* SGraphXPortDiffPanel::FindGraphForSide(const FSideData& Side, FName GraphName) const
{
    if (!Side.Blueprint || GraphName.IsNone())
    {
        return nullptr;
    }

    TArray<UEdGraph*> Graphs;
    Side.Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && Graph->GetFName() == GraphName)
        {
            return Graph;
        }
    }

    return nullptr;
}

UEdGraphNode* SGraphXPortDiffPanel::FindNodeForSide(const FSideData& Side, FName GraphName, const FGuid& NodeGuid) const
{
    if (!NodeGuid.IsValid())
    {
        return nullptr;
    }

    UEdGraph* Graph = FindGraphForSide(Side, GraphName);
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == NodeGuid)
        {
            return Node;
        }
    }

    return nullptr;
}

void SGraphXPortDiffPanel::ComputeAllDiffs()
{
    GraphDiffs.Reset();

    for (const FName& GraphName : GraphNames)
    {
        FGraphDiffSet Set;
        Set.MineDiffs = MakeShared<TArray<FDiffSingleResult>>();
        Set.TheirsDiffs = MakeShared<TArray<FDiffSingleResult>>();
        Set.ResultDiffs = MakeShared<TArray<FDiffSingleResult>>();
        Set.BaseDiffs = MakeShared<TArray<FDiffSingleResult>>();

        UEdGraph* BaseGraph = FindGraphForSide(BaseSide, GraphName);
        UEdGraph* MineGraph = FindGraphForSide(MineSide, GraphName);
        UEdGraph* TheirsGraph = FindGraphForSide(TheirsSide, GraphName);
        UEdGraph* ResultGraph = FindGraphForSide(ResultSide, GraphName);

        // Each side is diffed against the common ancestor, so every pane shows what that
        // side changed rather than a single combined mine-vs-theirs delta.
        if (BaseGraph && MineGraph)
        {
            FGraphDiffControl::DiffGraphs(BaseGraph, MineGraph, *Set.MineDiffs);
        }
        if (BaseGraph && TheirsGraph)
        {
            FGraphDiffControl::DiffGraphs(BaseGraph, TheirsGraph, *Set.TheirsDiffs);
        }
        if (BaseGraph && ResultGraph)
        {
            FGraphDiffControl::DiffGraphs(BaseGraph, ResultGraph, *Set.ResultDiffs);
        }
        // The base pane marks anything either side touched.
        if (Mode == EGraphXPortDiffMode::Conflict3Way)
        {
            Set.BaseDiffs->Append(*Set.MineDiffs);
            Set.BaseDiffs->Append(*Set.TheirsDiffs);
        }
        else
        {
            // The HEAD pane highlights what the working tree changed.
            Set.BaseDiffs->Append(*Set.ResultDiffs);
        }

        GraphDiffs.Add(GraphName, Set);
    }
}

void SGraphXPortDiffPanel::AppendVariableDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (!BaseSide.Blueprint)
    {
        return;
    }

    auto CompareVariables = [&OutRows](const UBlueprint* Base, const UBlueprint* Side, const FText& SideTag, bool bFromTheirs)
    {
        if (!Base || !Side)
        {
            return;
        }

        for (const FBPVariableDescription& SideVar : Side->NewVariables)
        {
            const FBPVariableDescription* BaseVar = Base->NewVariables.FindByPredicate(
                [&SideVar](const FBPVariableDescription& Candidate) { return Candidate.VarName == SideVar.VarName; });

            TSharedPtr<FGraphXPortDiffTreeNode> Row;
            if (!BaseVar)
            {
                Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("VarAdded", "{0}: Added variable '{1}'"), SideTag, FText::FromName(SideVar.VarName));
                Row->Color = AddedColor;
            }
            else if (BaseVar->VarType != SideVar.VarType)
            {
                Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("VarTypeChanged", "{0}: Variable '{1}' changed type"), SideTag, FText::FromName(SideVar.VarName));
                Row->Color = ChangedColor;
            }
            else if (BaseVar->DefaultValue != SideVar.DefaultValue)
            {
                Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("VarDefaultChanged", "{0}: Variable '{1}' default changed"), SideTag, FText::FromName(SideVar.VarName));
                Row->Color = ChangedColor;
            }

            if (Row.IsValid())
            {
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                OutRows.Add(Row);
            }
        }

        for (const FBPVariableDescription& BaseVar : Base->NewVariables)
        {
            const bool bStillPresent = Side->NewVariables.ContainsByPredicate(
                [&BaseVar](const FBPVariableDescription& Candidate) { return Candidate.VarName == BaseVar.VarName; });

            if (!bStillPresent)
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("VarRemoved", "{0}: Removed variable '{1}'"), SideTag, FText::FromName(BaseVar.VarName));
                Row->Color = RemovedColor;
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                OutRows.Add(Row);
            }
        }
    };

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    CompareVariables(BaseSide.Blueprint, LocalChangeSide().Blueprint, MineTag, /*bFromTheirs=*/false);
    CompareVariables(BaseSide.Blueprint, TheirsSide.Blueprint, LOCTEXT("SideTheirs", "THEIRS"), /*bFromTheirs=*/true);
}

void SGraphXPortDiffPanel::AppendDefaultsDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (!BaseSide.Blueprint)
    {
        return;
    }

    const UObject* BaseCDO = DiffUtils::GetCDO(BaseSide.Blueprint);
    if (!BaseCDO)
    {
        return;
    }

    auto CompareDefaults = [&OutRows, BaseCDO](const UBlueprint* SideBlueprint, const FText& SideTag, bool bFromTheirs)
    {
        if (!SideBlueprint)
        {
            return;
        }

        const UObject* SideCDO = DiffUtils::GetCDO(SideBlueprint);
        if (!SideCDO)
        {
            return;
        }

        TArray<FSingleObjectDiffEntry> Differences;
        DiffUtils::CompareUnrelatedObjects(BaseCDO, SideCDO, Differences);

        for (const FSingleObjectDiffEntry& Difference : Differences)
        {
            TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
            const FText PropertyName = FText::FromString(Difference.Identifier.ToDisplayName());

            switch (Difference.DiffType)
            {
            case EPropertyDiffType::PropertyAddedToB:
                Row->Label = FText::Format(LOCTEXT("DefaultAdded", "{0}: Added '{1}'"), SideTag, PropertyName);
                Row->Color = AddedColor;
                break;
            case EPropertyDiffType::PropertyAddedToA:
                Row->Label = FText::Format(LOCTEXT("DefaultRemoved", "{0}: Removed '{1}'"), SideTag, PropertyName);
                Row->Color = RemovedColor;
                break;
            default:
                Row->Label = FText::Format(LOCTEXT("DefaultChanged", "{0}: Changed '{1}'"), SideTag, PropertyName);
                Row->Color = ChangedColor;
                break;
            }

            Row->bIsMemberRow = true;
            Row->bFromTheirs = bFromTheirs;
            OutRows.Add(Row);
        }
    };

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    CompareDefaults(LocalChangeSide().Blueprint, MineTag, /*bFromTheirs=*/false);
    CompareDefaults(TheirsSide.Blueprint, LOCTEXT("SideTheirs", "THEIRS"), /*bFromTheirs=*/true);
}

void SGraphXPortDiffPanel::AppendComponentDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (!BaseSide.Blueprint)
    {
        return;
    }

    auto CompareComponents = [&OutRows](const UBlueprint* Base, const UBlueprint* Side, const FText& SideTag, bool bFromTheirs)
    {
        if (!Base || !Side)
        {
            return;
        }

        TMap<FName, USCS_Node*> BaseNodes = GatherComponentNodes(Base);
        TMap<FName, USCS_Node*> SideNodes = GatherComponentNodes(Side);

        for (const TPair<FName, USCS_Node*>& SidePair : SideNodes)
        {
            USCS_Node** BaseNode = BaseNodes.Find(SidePair.Key);
            if (!BaseNode)
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("CompAdded", "{0}: Added component '{1}'"), SideTag, FText::FromName(SidePair.Key));
                Row->Color = AddedColor;
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                Row->ComponentName = SidePair.Key;
                OutRows.Add(Row);
                continue;
            }

            if ((*BaseNode)->ComponentClass != SidePair.Value->ComponentClass)
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("CompClassChanged", "{0}: Component '{1}' changed class"), SideTag, FText::FromName(SidePair.Key));
                Row->Color = ChangedColor;
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                Row->ComponentName = SidePair.Key;
                OutRows.Add(Row);
                continue;
            }

            // Same component on both sides - report changed properties on its template.
            const UActorComponent* BaseTemplate = (*BaseNode)->ComponentTemplate;
            const UActorComponent* SideTemplate = SidePair.Value->ComponentTemplate;
            if (BaseTemplate && SideTemplate)
            {
                TArray<FSingleObjectDiffEntry> Differences;
                DiffUtils::CompareUnrelatedObjects(BaseTemplate, SideTemplate, Differences);

                for (const FSingleObjectDiffEntry& Difference : Differences)
                {
                    TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                    Row->Label = FText::Format(
                        LOCTEXT("CompPropChanged", "{0}: Component '{1}' changed '{2}'"),
                        SideTag, FText::FromName(SidePair.Key), FText::FromString(Difference.Identifier.ToDisplayName()));
                    Row->Color = ChangedColor;
                    Row->bIsMemberRow = true;
                    Row->bFromTheirs = bFromTheirs;
                    Row->ComponentName = SidePair.Key;
                    OutRows.Add(Row);
                }
            }
        }

        for (const TPair<FName, USCS_Node*>& BasePair : BaseNodes)
        {
            if (!SideNodes.Contains(BasePair.Key))
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("CompRemoved", "{0}: Removed component '{1}'"), SideTag, FText::FromName(BasePair.Key));
                Row->Color = RemovedColor;
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                Row->ComponentName = BasePair.Key;
                OutRows.Add(Row);
            }
        }
    };

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    CompareComponents(BaseSide.Blueprint, LocalChangeSide().Blueprint, MineTag, /*bFromTheirs=*/false);
    CompareComponents(BaseSide.Blueprint, TheirsSide.Blueprint, LOCTEXT("SideTheirs", "THEIRS"), /*bFromTheirs=*/true);
}

void SGraphXPortDiffPanel::AppendEntryDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (!BaseSide.Asset)
    {
        return;
    }

    auto CompareEntries = [&OutRows](const TArray<FGraphXPortAssetEntry>& Base, const TArray<FGraphXPortAssetEntry>& Side, const FText& SideTag, bool bFromTheirs)
    {
        for (const FGraphXPortAssetEntry& SideEntry : Side)
        {
            const FGraphXPortAssetEntry* BaseEntry = Base.FindByPredicate(
                [&SideEntry](const FGraphXPortAssetEntry& Candidate) { return Candidate.Key == SideEntry.Key; });

            TSharedPtr<FGraphXPortDiffTreeNode> Row;
            if (!BaseEntry)
            {
                Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("EntryAdded", "{0}: Added '{1}'"), SideTag, FText::FromString(SideEntry.Display));
                Row->Color = AddedColor;
            }
            else if (BaseEntry->Value != SideEntry.Value)
            {
                Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("EntryChanged", "{0}: Changed '{1}'"), SideTag, FText::FromString(SideEntry.Display));
                Row->Color = ChangedColor;
            }

            if (Row.IsValid())
            {
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                OutRows.Add(Row);
            }
        }

        for (const FGraphXPortAssetEntry& BaseEntry : Base)
        {
            const bool bStillPresent = Side.ContainsByPredicate(
                [&BaseEntry](const FGraphXPortAssetEntry& Candidate) { return Candidate.Key == BaseEntry.Key; });

            if (!bStillPresent)
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("EntryRemoved", "{0}: Removed '{1}'"), SideTag, FText::FromString(BaseEntry.Display));
                Row->Color = RemovedColor;
                Row->bIsMemberRow = true;
                Row->bFromTheirs = bFromTheirs;
                OutRows.Add(Row);
            }
        }
    };

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    if (LocalChangeSide().Asset)
    {
        CompareEntries(BaseSide.Entries, LocalChangeSide().Entries, MineTag, /*bFromTheirs=*/false);
    }
    if (TheirsSide.Asset)
    {
        CompareEntries(BaseSide.Entries, TheirsSide.Entries, LOCTEXT("SideTheirs", "THEIRS"), /*bFromTheirs=*/true);
    }
}

void SGraphXPortDiffPanel::AppendGraphPresenceDiffs(FName GraphName, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (!BaseSide.Blueprint)
    {
        return;
    }

    const bool bInBase = FindGraphForSide(BaseSide, GraphName) != nullptr;

    auto ComparePresence = [&OutRows, &GraphName, bInBase, this](const FSideData& Side, const FText& SideTag)
    {
        if (!Side.Blueprint)
        {
            return;
        }

        const bool bInSide = FindGraphForSide(Side, GraphName) != nullptr;
        if (bInBase == bInSide)
        {
            return;
        }

        TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
        Row->GraphName = GraphName;
        Row->Label = bInSide
            ? FText::Format(LOCTEXT("GraphAdded", "{0}: Added"), SideTag)
            : FText::Format(LOCTEXT("GraphRemoved", "{0}: Removed"), SideTag);
        Row->Color = bInSide ? AddedColor : RemovedColor;
        OutRows.Add(Row);
    };

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    ComparePresence(LocalChangeSide(), MineTag);
    ComparePresence(TheirsSide, LOCTEXT("SideTheirs", "THEIRS"));
}

void SGraphXPortDiffPanel::BuildDiffTree()
{
    using namespace GraphXPortDiffPanelPrivate;

    DiffTreeRoots.Reset();

    const FText MineTag = Mode == EGraphXPortDiffMode::Conflict3Way
        ? LOCTEXT("SideMine", "MINE")
        : LOCTEXT("SideWorking", "WORKING");

    // Every change, whatever section it came from, is gathered flat first; the tree is then
    // partitioned into Conflicts / Mine Only / Theirs Only.
    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> AllRows;

    auto Gather = [&AllRows](TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& Rows, const FText& CategoryLabel)
    {
        for (const TSharedPtr<FGraphXPortDiffTreeNode>& Row : Rows)
        {
            Row->CategoryLabel = CategoryLabel;
            AllRows.Add(Row);
        }
    };

    // Enums, structs and data tables have no graphs at all - they are compared as a flat
    // list of entries.
    if (FGraphXPortAssetDiff::IsEntryBased(ResultSide.Kind) || FGraphXPortAssetDiff::IsEntryBased(BaseSide.Kind))
    {
        TArray<TSharedPtr<FGraphXPortDiffTreeNode>> EntryRows;
        AppendEntryDiffs(EntryRows);

        const EGraphXPortAssetKind Kind = ResultSide.Asset ? ResultSide.Kind : BaseSide.Kind;
        Gather(EntryRows, FGraphXPortAssetDiff::DescribeEntries(Kind));

        PartitionRowsIntoTree(AllRows);
        return;
    }

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> VariableRows;
    AppendVariableDiffs(VariableRows);
    Gather(VariableRows, LOCTEXT("VariablesSection", "Variables"));

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> ComponentRows;
    AppendComponentDiffs(ComponentRows);
    Gather(ComponentRows, LOCTEXT("ComponentsSection", "Components"));

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> DefaultsRows;
    AppendDefaultsDiffs(DefaultsRows);
    Gather(DefaultsRows, LOCTEXT("DefaultsSection", "Defaults"));

    for (const FName& GraphName : GraphNames)
    {
        const FGraphDiffSet* Set = GraphDiffs.Find(GraphName);
        if (!Set)
        {
            continue;
        }

        TArray<TSharedPtr<FGraphXPortDiffTreeNode>> GraphRows;

        // A graph that only exists on one side produces no node-level diffs, so surface
        // its presence explicitly.
        AppendGraphPresenceDiffs(GraphName, GraphRows);

        auto AddRows = [&GraphRows, &GraphName](const TArray<FDiffSingleResult>& Results, const FText& SideTag, bool bFromTheirs)
        {
            for (const FDiffSingleResult& Result : Results)
            {
                TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
                Row->Label = FText::Format(LOCTEXT("DiffRow", "{0}: {1}"), SideTag, Result.DisplayString);
                Row->Color = Result.GetDisplayColor();
                Row->GraphName = GraphName;
                Row->BaseNode = Result.Node1;
                Row->SideNode = Result.Node2;
                Row->bFromTheirs = bFromTheirs;

                // Prefer the ancestor node's identity: it exists in every revision, so all
                // three panes can focus it. A node added by one side only resolves there.
                const UEdGraphNode* IdentityNode = Result.Node1 ? Result.Node1 : Result.Node2;
                if (IdentityNode)
                {
                    Row->NodeGuid = IdentityNode->NodeGuid;
                }
                Row->bCanJump = Row->NodeGuid.IsValid();

                GraphRows.Add(Row);
            }
        };

        // Outside a conflict there are no git stages: the working tree is the only side
        // that changed, so its diff against HEAD is what the tree lists.
        const bool bConflict = Mode == EGraphXPortDiffMode::Conflict3Way;
        AddRows(bConflict ? *Set->MineDiffs : *Set->ResultDiffs, MineTag, /*bFromTheirs=*/false);
        if (bConflict)
        {
            AddRows(*Set->TheirsDiffs, LOCTEXT("SideTheirs", "THEIRS"), /*bFromTheirs=*/true);
        }

        Gather(GraphRows, FText::FromName(GraphName));
    }

    PartitionRowsIntoTree(AllRows);
}

void SGraphXPortDiffPanel::PartitionRowsIntoTree(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& AllRows)
{
    using namespace GraphXPortDiffPanelPrivate;

    // Groups rows under a section by their category label, preserving discovery order.
    auto GroupInto = [](const TSharedPtr<FGraphXPortDiffTreeNode>& Parent,
        const TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& Rows)
    {
        TMap<FString, TSharedPtr<FGraphXPortDiffTreeNode>> Groups;
        for (const TSharedPtr<FGraphXPortDiffTreeNode>& Row : Rows)
        {
            const FString GroupKey = Row->CategoryLabel.ToString();
            TSharedPtr<FGraphXPortDiffTreeNode>& Group = Groups.FindOrAdd(GroupKey);
            if (!Group.IsValid())
            {
                Group = MakeShared<FGraphXPortDiffTreeNode>();
                Group->bIsCategory = true;
                Group->Color = CategoryColor;
                Group->Label = Row->CategoryLabel;
                Group->GraphName = Row->GraphName;
                Group->bIsMemberRow = Row->bIsMemberRow;
                Parent->Children.Add(Group);
            }
            Group->Children.Add(Row);
        }
    };

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> MineOnly;
    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> TheirsOnly;
    for (const TSharedPtr<FGraphXPortDiffTreeNode>& Row : AllRows)
    {
        (Row->bFromTheirs ? TheirsOnly : MineOnly).Add(Row);
    }

    TSharedPtr<FGraphXPortDiffTreeNode> MineSection = MakeShared<FGraphXPortDiffTreeNode>();
    MineSection->bIsCategory = true;
    MineSection->Color = CategoryColor;
    MineSection->Label = FText::Format(LOCTEXT("MineChangesSection", "Mine Changes ({0})"), FText::AsNumber(MineOnly.Num()));
    GroupInto(MineSection, MineOnly);

    TSharedPtr<FGraphXPortDiffTreeNode> TheirsSection = MakeShared<FGraphXPortDiffTreeNode>();
    TheirsSection->bIsCategory = true;
    TheirsSection->Color = CategoryColor;
    TheirsSection->Label = FText::Format(LOCTEXT("TheirsChangesSection", "Theirs Changes ({0})"), FText::AsNumber(TheirsOnly.Num()));
    GroupInto(TheirsSection, TheirsOnly);

    if (Mode == EGraphXPortDiffMode::Conflict3Way)
    {
        DiffTreeRoots.Add(MineSection);
        DiffTreeRoots.Add(TheirsSection);
    }
    else
    {
        // Outside a conflict there is only one side that changed anything.
        MineSection->Label = FText::Format(LOCTEXT("ChangesSection", "Changes ({0})"), FText::AsNumber(MineOnly.Num()));
        DiffTreeRoots.Add(MineSection);
    }

    if (DiffTreeView.IsValid())
    {
        DiffTreeView->RequestTreeRefresh();
        ExpandAllTreeItems();
    }
}

void SGraphXPortDiffPanel::ExpandAllTreeItems()
{
    if (!DiffTreeView.IsValid())
    {
        return;
    }

    for (const TSharedPtr<FGraphXPortDiffTreeNode>& Root : DiffTreeRoots)
    {
        DiffTreeView->SetItemExpansion(Root, true);
        for (const TSharedPtr<FGraphXPortDiffTreeNode>& Group : Root->Children)
        {
            DiffTreeView->SetItemExpansion(Group, true);
        }
    }
}

void SGraphXPortDiffPanel::ApplySelectedGraphDiffResults()
{
    const FGraphDiffSet* Set = GraphDiffs.Find(SelectedGraphName);

    BaseSide.DiffResults = Set ? Set->BaseDiffs : nullptr;
    MineSide.DiffResults = Set ? Set->MineDiffs : nullptr;
    TheirsSide.DiffResults = Set ? Set->TheirsDiffs : nullptr;
    ResultSide.DiffResults = Set ? Set->ResultDiffs : nullptr;
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildToolbarRow()
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(6.f, 2.f)
        [
            SNew(STextBlock)
            .Text(this, &SGraphXPortDiffPanel::GetCurrentGraphLabel)
            .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(12.f, 2.f)
        .VAlign(VAlign_Center)
        [
            SNew(SButton)
            .Text(LOCTEXT("Save", "Save Mine"))
            .ToolTipText(LOCTEXT("SaveTip", "Compile and save the working-tree asset."))
            .Visibility(this, &SGraphXPortDiffPanel::GetPlainSaveVisibility)
            .OnClicked(this, &SGraphXPortDiffPanel::OnSaveClicked)
            .IsEnabled_Lambda([this]() { return ResultSide.Asset != nullptr && ResultSide.bEditable; })
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(12.f, 2.f)
        .VAlign(VAlign_Center)
        [
            SNew(SButton)
            .Text(LOCTEXT("SaveResolve", "Save && Mark Resolved"))
            .ToolTipText(LOCTEXT("SaveResolveTip", "Save the merged result you edited in the Mine pane, then stage it so git records the conflict as resolved."))
            .Visibility(this, &SGraphXPortDiffPanel::GetConflictActionVisibility)
            .OnClicked(this, &SGraphXPortDiffPanel::OnSaveAndResolveClicked)
            .IsEnabled_Lambda([this]() { return ResultSide.Asset != nullptr && ResultSide.bEditable; })
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(12.f, 2.f)
        .VAlign(VAlign_Center)
        [
            SNew(SButton)
            .Text(this, &SGraphXPortDiffPanel::GetLeftToggleLabel)
            .ToolTipText(LOCTEXT("ToggleLeftTip", "Switch the left pane between the common ancestor and your own side."))
            .Visibility(this, &SGraphXPortDiffPanel::GetConflictActionVisibility)
            .OnClicked(this, &SGraphXPortDiffPanel::OnToggleLeftPaneClicked)
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .VAlign(VAlign_Center)
        .Padding(12.f, 2.f)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(FText::GetEmpty())
        ];
}

FText SGraphXPortDiffPanel::GetLeftToggleLabel() const
{
    return bLeftShowsMine
        ? LOCTEXT("ShowingMine", "Left: Mine")
        : LOCTEXT("ShowingBase", "Left: Base");
}

FReply SGraphXPortDiffPanel::OnToggleLeftPaneClicked()
{
    bLeftShowsMine = !bLeftShowsMine;
    RebuildPanes();
    return FReply::Handled();
}

EVisibility SGraphXPortDiffPanel::GetConflictActionVisibility() const
{
    return Mode == EGraphXPortDiffMode::Conflict3Way ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SGraphXPortDiffPanel::GetPlainSaveVisibility() const
{
    return Mode == EGraphXPortDiffMode::Conflict3Way ? EVisibility::Collapsed : EVisibility::Visible;
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildDiffTreeWidget()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f, 2.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("Differences", "Differences"))
            .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(DiffTreeView, STreeView<TSharedPtr<FGraphXPortDiffTreeNode>>)
            .TreeItemsSource(&DiffTreeRoots)
            .OnGenerateRow(this, &SGraphXPortDiffPanel::GenerateTreeRow)
            .OnGetChildren(this, &SGraphXPortDiffPanel::OnGetTreeChildren)
            .OnSelectionChanged(this, &SGraphXPortDiffPanel::OnTreeSelectionChanged)
            .SelectionMode(ESelectionMode::Single)
        ];
}

TSharedRef<ITableRow> SGraphXPortDiffPanel::GenerateTreeRow(TSharedPtr<FGraphXPortDiffTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(STableRow<TSharedPtr<FGraphXPortDiffTreeNode>>, OwnerTable)
        [
            SNew(STextBlock)
            .Text(Item->Label)
            .ColorAndOpacity(FSlateColor(Item->Color))
            .Font(Item->bIsCategory
                ? FAppStyle::Get().GetFontStyle("BoldFont")
                : FAppStyle::Get().GetFontStyle("NormalFont"))
        ];
}

void SGraphXPortDiffPanel::OnGetTreeChildren(TSharedPtr<FGraphXPortDiffTreeNode> Item, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutChildren)
{
    if (Item.IsValid())
    {
        OutChildren = Item->Children;
    }
}

void SGraphXPortDiffPanel::OnTreeSelectionChanged(TSharedPtr<FGraphXPortDiffTreeNode> Item, ESelectInfo::Type SelectInfo)
{
    if (!Item.IsValid())
    {
        return;
    }

    // Variables, defaults and components have no graph to show - swap the panes over to
    // the member lists instead.
    if (Item->bIsMemberRow)
    {
        const bool bChanged = !bShowMembers || InspectedComponentName != Item->ComponentName;
        bShowMembers = true;
        InspectedComponentName = Item->ComponentName;
        if (bChanged)
        {
            RebuildPanes();
        }
        return;
    }

    if (Item->GraphName.IsNone())
    {
        return;
    }

    bool bNeedsRebuild = bShowMembers;
    bShowMembers = false;

    if (Item->GraphName != SelectedGraphName)
    {
        SelectedGraphName = Item->GraphName;
        ApplySelectedGraphDiffResults();
        bNeedsRebuild = true;
    }

    if (bNeedsRebuild)
    {
        RebuildPanes();
    }

    if (!Item->bCanJump)
    {
        return;
    }

    // A MINE change lives in the Mine reference, so flip the left pane to it rather than
    // focusing a pane the user cannot currently see.
    if (!Item->bFromTheirs && Mode == EGraphXPortDiffMode::Conflict3Way && !bLeftShowsMine)
    {
        bLeftShowsMine = true;
        RebuildPanes();
    }

    RequestJumpToNode(Item->GraphName, Item->NodeGuid);
}

void SGraphXPortDiffPanel::RequestJumpToNode(FName GraphName, const FGuid& NodeGuid)
{
    if (!NodeGuid.IsValid())
    {
        return;
    }

    PendingJumpGraphName = GraphName;
    PendingJumpNodeGuid = NodeGuid;

    if (!bJumpTimerRegistered)
    {
        bJumpTimerRegistered = true;
        RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SGraphXPortDiffPanel::HandlePendingJump));
    }
}

EActiveTimerReturnType SGraphXPortDiffPanel::HandlePendingJump(double InCurrentTime, float InDeltaTime)
{
    bJumpTimerRegistered = false;

    const FGuid NodeGuid = PendingJumpNodeGuid;
    const FName GraphName = PendingJumpGraphName;
    PendingJumpNodeGuid.Invalidate();

    if (!NodeGuid.IsValid())
    {
        return EActiveTimerReturnType::Stop;
    }

    // Every pane focuses the same node resolved in its own revision, so the three views stay
    // lined up on whatever the row refers to. A pane whose revision lacks the node is left
    // where it is rather than jumping somewhere arbitrary.
    auto FocusPane = [this, &GraphName, &NodeGuid](const TSharedPtr<SGraphEditor>& Pane, const FSideData& Side)
    {
        if (!Pane.IsValid())
        {
            return;
        }
        if (UEdGraphNode* Node = FindNodeForSide(Side, GraphName, NodeGuid))
        {
            Pane->JumpToNode(Node, /*bRequestRename=*/false, /*bSelectNode=*/true);
        }
    };

    FocusPane(LeftPane, bLeftShowsMine ? MineSide : BaseSide);
    FocusPane(ResultPane, ResultSide);
    FocusPane(TheirsPane, TheirsSide);

    return EActiveTimerReturnType::Stop;
}

FText SGraphXPortDiffPanel::GetCurrentGraphLabel() const
{
    return SelectedGraphName.IsNone()
        ? LOCTEXT("NoGraphSelected", "No graph selected")
        : FText::FromName(SelectedGraphName);
}

void SGraphXPortDiffPanel::CopySelectedNodes(const TSharedPtr<SGraphEditor>* Pane)
{
    if (!Pane || !Pane->IsValid())
    {
        return;
    }

    FString Exported;
    FEdGraphUtilities::ExportNodesToText((*Pane)->GetSelectedNodes(), Exported);
    if (!Exported.IsEmpty())
    {
        FPlatformApplicationMisc::ClipboardCopy(*Exported);
    }
}

bool SGraphXPortDiffPanel::CanPasteIntoMine() const
{
    if (!ResultPane.IsValid() || !ResultSide.bEditable)
    {
        return false;
    }

    UEdGraph* Graph = ResultPane->GetCurrentGraph();
    if (!Graph)
    {
        return false;
    }

    FString ClipboardText;
    FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
    return FEdGraphUtilities::CanImportNodesFromText(Graph, ClipboardText);
}

void SGraphXPortDiffPanel::PasteNodesIntoMine()
{
    if (!ResultPane.IsValid() || !ResultSide.bEditable || !ResultSide.Blueprint)
    {
        return;
    }

    UEdGraph* Graph = ResultPane->GetCurrentGraph();
    if (!Graph)
    {
        return;
    }

    FString TextToImport;
    FPlatformApplicationMisc::ClipboardPaste(TextToImport);
    if (!FEdGraphUtilities::CanImportNodesFromText(Graph, TextToImport))
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("PasteNodes", "Paste Nodes"));
    Graph->Modify();
    ResultPane->ClearSelectionSet();

    TSet<UEdGraphNode*> PastedNodes;
    FEdGraphUtilities::ImportNodesFromText(Graph, TextToImport, PastedNodes);

    // Drop the pasted block where the user is looking, preserving its internal layout.
    FVector2D AvgPosition(0.f, 0.f);
    for (const UEdGraphNode* Node : PastedNodes)
    {
        AvgPosition.X += Node->NodePosX;
        AvgPosition.Y += Node->NodePosY;
    }
    if (PastedNodes.Num() > 0)
    {
        AvgPosition /= static_cast<float>(PastedNodes.Num());
    }

    const FVector2D PasteLocation = ResultPane->GetPasteLocation();
    for (UEdGraphNode* Node : PastedNodes)
    {
        ResultPane->SetNodeSelection(Node, true);
        Node->NodePosX = (Node->NodePosX - AvgPosition.X) + PasteLocation.X;
        Node->NodePosY = (Node->NodePosY - AvgPosition.Y) + PasteLocation.Y;
        Node->SnapToGrid(16);
        // Pasted nodes must not share identity with the ones they were copied from.
        Node->CreateNewGuid();
    }

    ResultPane->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(ResultSide.Blueprint.Get());
}

void SGraphXPortDiffPanel::DeleteSelectedMineNodes()
{
    if (!ResultPane.IsValid() || !ResultSide.bEditable || !ResultSide.Blueprint)
    {
        return;
    }

    UEdGraph* Graph = ResultPane->GetCurrentGraph();
    if (!Graph)
    {
        return;
    }

    const FGraphPanelSelectionSet Selected = ResultPane->GetSelectedNodes();
    if (Selected.Num() == 0)
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("DeleteNodes", "Delete Nodes"));
    Graph->Modify();

    for (UObject* SelectedObject : Selected)
    {
        UEdGraphNode* Node = Cast<UEdGraphNode>(SelectedObject);
        if (Node && Node->CanUserDeleteNode())
        {
            Node->Modify();
            Node->DestroyNode();
        }
    }

    ResultPane->ClearSelectionSet();
    ResultPane->NotifyGraphChanged();
    FBlueprintEditorUtils::MarkBlueprintAsModified(ResultSide.Blueprint.Get());
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildMembersWidget(const FSideData& Side) const
{
    using namespace GraphXPortDiffPanelPrivate;

    TSharedRef<SVerticalBox> List = SNew(SVerticalBox);

    if (!Side.Blueprint)
    {
        return List;
    }

    const bool bIsBase = &Side == &BaseSide;

    // Section headers use the editor's own category bar so the list reads like a
    // details/My Blueprint panel rather than a wall of text.
    auto AddSectionHeader = [&List](const FText& Title)
    {
        List->AddSlot()
        .AutoHeight()
        .Padding(0.f, 6.f, 0.f, 0.f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("DetailsView.CategoryTop"))
            .Padding(FMargin(8.f, 4.f))
            [
                SNew(STextBlock)
                .Text(Title)
                .Font(FAppStyle::Get().GetFontStyle("DetailsView.CategoryFontStyle"))
                .ShadowOffset(FVector2D(1.f, 1.f))
            ]
        ];
    };

    auto AddRow = [&List](const FText& Text, const FLinearColor& Color)
    {
        List->AddSlot()
        .AutoHeight()
        .Padding(20.f, 3.f, 8.f, 3.f)
        [
            SNew(STextBlock)
            .Text(Text)
            .ColorAndOpacity(FSlateColor(Color))
        ];
    };

    // FUNCTIONS / GRAPHS
    AddSectionHeader(LOCTEXT("MembersGraphs", "GRAPHS & FUNCTIONS"));
    for (const FName& GraphName : GraphNames)
    {
        if (!FindGraphForSide(Side, GraphName))
        {
            continue;
        }

        FLinearColor Color = UnchangedColor;
        if (bIsBase)
        {
            const bool bMissingSomewhere =
                (MineSide.Blueprint && !FindGraphForSide(MineSide, GraphName)) ||
                (TheirsSide.Blueprint && !FindGraphForSide(TheirsSide, GraphName));
            Color = bMissingSomewhere ? RemovedColor : UnchangedColor;
        }
        else if (BaseSide.Blueprint && !FindGraphForSide(BaseSide, GraphName))
        {
            Color = AddedColor;
        }

        AddRow(FText::FromName(GraphName), Color);
    }

    // VARIABLES
    AddSectionHeader(LOCTEXT("MembersVariables", "VARIABLES"));
    for (const FBPVariableDescription& Variable : Side.Blueprint->NewVariables)
    {
        FLinearColor Color = UnchangedColor;

        auto FindVar = [&Variable](const UBlueprint* In) -> const FBPVariableDescription*
        {
            return In ? In->NewVariables.FindByPredicate(
                [&Variable](const FBPVariableDescription& Candidate) { return Candidate.VarName == Variable.VarName; }) : nullptr;
        };

        if (bIsBase)
        {
            const bool bMissingSomewhere =
                (MineSide.Blueprint && !FindVar(MineSide.Blueprint)) ||
                (TheirsSide.Blueprint && !FindVar(TheirsSide.Blueprint));
            Color = bMissingSomewhere ? RemovedColor : UnchangedColor;
        }
        else if (BaseSide.Blueprint)
        {
            const FBPVariableDescription* BaseVar = FindVar(BaseSide.Blueprint);
            if (!BaseVar)
            {
                Color = AddedColor;
            }
            else if (BaseVar->VarType != Variable.VarType || BaseVar->DefaultValue != Variable.DefaultValue)
            {
                Color = ChangedColor;
            }
        }

        AddRow(FText::Format(LOCTEXT("MemberVarRow", "{0}  :  {1}"),
            FText::FromName(Variable.VarName), DescribeVariableType(Variable)), Color);
    }

    // COMPONENTS
    AddSectionHeader(LOCTEXT("MembersComponents", "COMPONENTS"));
    const TMap<FName, USCS_Node*> SideNodes = GatherComponentNodes(Side.Blueprint);
    const TMap<FName, USCS_Node*> BaseNodes = GatherComponentNodes(BaseSide.Blueprint);
    const TMap<FName, USCS_Node*> MineNodes = GatherComponentNodes(MineSide.Blueprint);
    const TMap<FName, USCS_Node*> TheirsNodes = GatherComponentNodes(TheirsSide.Blueprint);

    for (const TPair<FName, USCS_Node*>& Pair : SideNodes)
    {
        FLinearColor Color = UnchangedColor;
        if (bIsBase)
        {
            const bool bMissingSomewhere =
                (MineSide.Blueprint && !MineNodes.Contains(Pair.Key)) ||
                (TheirsSide.Blueprint && !TheirsNodes.Contains(Pair.Key));
            Color = bMissingSomewhere ? RemovedColor : UnchangedColor;
        }
        else if (BaseSide.Blueprint)
        {
            USCS_Node* const* BaseNode = BaseNodes.Find(Pair.Key);
            if (!BaseNode)
            {
                Color = AddedColor;
            }
            else if ((*BaseNode)->ComponentClass != Pair.Value->ComponentClass)
            {
                Color = ChangedColor;
            }
        }

        const FString ClassName = Pair.Value->ComponentClass ? Pair.Value->ComponentClass->GetName() : TEXT("None");
        AddRow(FText::Format(LOCTEXT("MemberCompRow", "{0}  :  {1}"),
            FText::FromName(Pair.Key), FText::FromString(ClassName)), Color);
    }

    // Below the member list, a real Details panel showing the actual values of whichever
    // component (or the class defaults) the selected difference refers to - so a change
    // to something like a tooltip or a component tag is visible, not just named.
    UObject* ObjectToInspect = nullptr;
    if (!InspectedComponentName.IsNone())
    {
        const TMap<FName, USCS_Node*> Nodes = GatherComponentNodes(Side.Blueprint);
        if (USCS_Node* const* Node = Nodes.Find(InspectedComponentName))
        {
            ObjectToInspect = (*Node)->ComponentTemplate;
        }
    }
    else if (Side.Blueprint->GeneratedClass)
    {
        ObjectToInspect = Side.Blueprint->GeneratedClass->GetDefaultObject();
    }

    FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

    FDetailsViewArgs DetailsArgs;
    DetailsArgs.bAllowSearch = false;
    DetailsArgs.bHideSelectionTip = true;
    DetailsArgs.bShowOptions = false;
    DetailsArgs.bLockable = false;
    DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

    TSharedRef<IDetailsView> DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
    if (!Side.bEditable)
    {
        DetailsView->SetIsPropertyEditingEnabledDelegate(
            FIsPropertyEditingEnabled::CreateLambda([]() { return false; }));
    }
    DetailsView->SetObject(ObjectToInspect);

    return SNew(SSplitter)
        .Orientation(Orient_Vertical)
        + SSplitter::Slot()
        .Value(0.45f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                List
            ]
        ]
        + SSplitter::Slot()
        .Value(0.55f)
        [
            DetailsView
        ];
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildEntryListWidget(const FSideData& Side) const
{
    using namespace GraphXPortDiffPanelPrivate;

    const bool bIsBase = &Side == &BaseSide;

    TSharedRef<SGridPanel> Grid = SNew(SGridPanel);

    auto MakeCell = [](const FString& Text, const FLinearColor& Color, bool bHeader)
    {
        return SNew(SBox)
            .MaxDesiredWidth(260.f)
            .Padding(FMargin(8.f, 3.f))
            [
                SNew(STextBlock)
                .Text(FText::FromString(Text))
                .ToolTipText(FText::FromString(Text))
                .ColorAndOpacity(FSlateColor(Color))
                .Font(bHeader
                    ? FAppStyle::Get().GetFontStyle("DetailsView.CategoryFontStyle")
                    : FAppStyle::Get().GetFontStyle("NormalFont"))
            ];
    };

    // Header row
    for (int32 ColumnIndex = 0; ColumnIndex < Side.Columns.Num(); ++ColumnIndex)
    {
        Grid->AddSlot(ColumnIndex, 0)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("DetailsView.CategoryTop"))
            [
                MakeCell(Side.Columns[ColumnIndex], CategoryColor, /*bHeader=*/true)
            ]
        ];
    }

    int32 Row = 1;
    for (const FGraphXPortAssetEntry& AssetEntry : Side.Entries)
    {
        auto Find = [&AssetEntry](const TArray<FGraphXPortAssetEntry>& In) -> const FGraphXPortAssetEntry*
        {
            return In.FindByPredicate(
                [&AssetEntry](const FGraphXPortAssetEntry& Candidate) { return Candidate.Key == AssetEntry.Key; });
        };

        const FGraphXPortAssetEntry* BaseEntry = BaseSide.Asset ? Find(BaseSide.Entries) : nullptr;

        FLinearColor RowColor = UnchangedColor;
        if (bIsBase)
        {
            const bool bMissingSomewhere =
                (MineSide.Asset && !Find(MineSide.Entries)) ||
                (TheirsSide.Asset && !Find(TheirsSide.Entries));
            RowColor = bMissingSomewhere ? RemovedColor : UnchangedColor;
        }
        else if (BaseSide.Asset && !BaseEntry)
        {
            RowColor = AddedColor;
        }

        for (int32 ColumnIndex = 0; ColumnIndex < AssetEntry.Cells.Num(); ++ColumnIndex)
        {
            // An unchanged row still highlights the individual cells that differ, so a
            // single edited value in a wide table is visible without hunting.
            FLinearColor CellColor = RowColor;
            if (RowColor == UnchangedColor && BaseEntry && !bIsBase
                && BaseEntry->Cells.IsValidIndex(ColumnIndex)
                && BaseEntry->Cells[ColumnIndex] != AssetEntry.Cells[ColumnIndex])
            {
                CellColor = ChangedColor;
            }

            Grid->AddSlot(ColumnIndex, Row)
            [
                MakeCell(AssetEntry.Cells[ColumnIndex], CellColor, /*bHeader=*/false)
            ];
        }

        ++Row;
    }

    // Wide tables scroll horizontally rather than being clipped by the pane.
    return SNew(SScrollBox)
        + SScrollBox::Slot()
        [
            SNew(SScrollBox)
            .Orientation(Orient_Horizontal)
            + SScrollBox::Slot()
            [
                Grid
            ]
        ];
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildSideWidget(const FSideData& Side, TSharedPtr<SGraphEditor>& OutPane, TSharedPtr<FUICommandList>& OutCommands)
{
    TSharedRef<SVerticalBox> Container = SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f, 2.f)
        [
            SNew(STextBlock)
            .Text(Side.Label)
            .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
        ];

    // Entry-based assets have no graphs; they are always shown as their entry list.
    if (FGraphXPortAssetDiff::IsEntryBased(Side.Kind) && Side.Asset)
    {
        Container->AddSlot()
        .FillHeight(1.f)
        [
            BuildEntryListWidget(Side)
        ];
        return Container;
    }

    if (!Side.Blueprint)
    {
        Container->AddSlot()
        .FillHeight(1.f)
        .VAlign(VAlign_Center)
        .HAlign(HAlign_Center)
        [
            SNew(STextBlock)
            .Text(Side.UnavailableReason.IsEmpty() ? LOCTEXT("Unavailable", "Not available.") : FText::FromString(Side.UnavailableReason))
        ];
        return Container;
    }

    if (bShowMembers)
    {
        Container->AddSlot()
        .FillHeight(1.f)
        [
            BuildMembersWidget(Side)
        ];
        return Container;
    }

    UEdGraph* FoundGraph = FindGraphForSide(Side, SelectedGraphName);
    if (!FoundGraph)
    {
        Container->AddSlot()
        .FillHeight(1.f)
        .VAlign(VAlign_Center)
        .HAlign(HAlign_Center)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("NoSuchGraph", "Graph does not exist in this revision."))
        ];
        return Container;
    }

    if (!Side.bEditable)
    {
        // SGraphEditor's IsEditable alone still permits pin/connection edits; the graph's
        // own bEditable flag is what gates those. Safe to clear here because read-only
        // sides are always throwaway diff-only revisions that are never saved.
        FoundGraph->bEditable = false;
    }

    FGraphAppearanceInfo Appearance;
    Appearance.CornerText = Side.Label;

    SGraphEditor::FGraphEditorEvents Events;
    OutCommands = MakeShared<FUICommandList>();
    const TSharedPtr<FUICommandList>& Commands = OutCommands;
    TSharedPtr<SGraphEditor>* PanePtr = &OutPane;

    // Every pane can copy - that is how nodes travel from Base/Theirs into Mine.
    Commands->MapAction(
        FGenericCommands::Get().Copy,
        FExecuteAction::CreateStatic(&SGraphXPortDiffPanel::CopySelectedNodes, static_cast<const TSharedPtr<SGraphEditor>*>(PanePtr)),
        FCanExecuteAction::CreateLambda([PanePtr]()
        {
            return PanePtr->IsValid() && (*PanePtr)->GetSelectedNodes().Num() > 0;
        }));

    if (Side.bEditable)
    {
        // Only Mine accepts paste and delete.
        Commands->MapAction(
            FGenericCommands::Get().Paste,
            FExecuteAction::CreateSP(this, &SGraphXPortDiffPanel::PasteNodesIntoMine),
            FCanExecuteAction::CreateSP(this, &SGraphXPortDiffPanel::CanPasteIntoMine));

        Commands->MapAction(
            FGenericCommands::Get().Delete,
            FExecuteAction::CreateSP(this, &SGraphXPortDiffPanel::DeleteSelectedMineNodes),
            FCanExecuteAction::CreateLambda([PanePtr]()
            {
                return PanePtr->IsValid() && (*PanePtr)->GetSelectedNodes().Num() > 0;
            }));

        Events.OnCreateNodeOrPinMenu = SGraphEditor::FOnCreateNodeOrPinMenu::CreateLambda(
            [](UEdGraph*, const UEdGraphNode*, const UEdGraphPin*, FMenuBuilder* MenuBuilder, bool)
            {
                MenuBuilder->AddMenuEntry(FGenericCommands::Get().Copy);
                MenuBuilder->AddMenuEntry(FGenericCommands::Get().Paste);
                MenuBuilder->AddMenuEntry(FGenericCommands::Get().Delete);
                return FActionMenuContent(MenuBuilder->MakeWidget());
            });
    }
    else
    {
        // Read-only panes offer copy only - no node creation or deletion entries.
        Events.OnCreateNodeOrPinMenu = SGraphEditor::FOnCreateNodeOrPinMenu::CreateLambda(
            [](UEdGraph*, const UEdGraphNode*, const UEdGraphPin*, FMenuBuilder* MenuBuilder, bool)
            {
                MenuBuilder->AddMenuEntry(FGenericCommands::Get().Copy);
                return FActionMenuContent(MenuBuilder->MakeWidget());
            });
    }

    Container->AddSlot()
    .FillHeight(1.f)
    [
        SAssignNew(OutPane, SGraphEditor)
        .AdditionalCommands(Commands)
        .IsEditable(Side.bEditable)
        .DisplayAsReadOnly(!Side.bEditable)
        .Appearance(Appearance)
        .GraphToEdit(FoundGraph)
        .DiffResults(Side.DiffResults)
        .GraphEvents(Events)
    ];

    return Container;
}

void SGraphXPortDiffPanel::RebuildPanes()
{
    if (!PanesContainer.IsValid())
    {
        return;
    }

    LeftPane.Reset();
    ResultPane.Reset();
    TheirsPane.Reset();
    LeftPaneCommands.Reset();
    ResultPaneCommands.Reset();
    TheirsPaneCommands.Reset();

    struct FActiveSide
    {
        const FSideData* Side;
        TSharedPtr<SGraphEditor>* Pane;
        TSharedPtr<FUICommandList>* Commands;
    };

    // The left pane shows whichever reference the toggle selects.
    const FSideData* LeftSide = bLeftShowsMine ? &MineSide : &BaseSide;

    TArray<FActiveSide> ActiveSides;
    switch (Mode)
    {
    case EGraphXPortDiffMode::Conflict3Way:
        ActiveSides.Add({ LeftSide, &LeftPane, &LeftPaneCommands });
        ActiveSides.Add({ &ResultSide, &ResultPane, &ResultPaneCommands });
        ActiveSides.Add({ &TheirsSide, &TheirsPane, &TheirsPaneCommands });
        break;
    case EGraphXPortDiffMode::ModifiedCompare:
        ActiveSides.Add({ &BaseSide, &LeftPane, &LeftPaneCommands });
        ActiveSides.Add({ &ResultSide, &ResultPane, &ResultPaneCommands });
        break;
    case EGraphXPortDiffMode::SingleReadOnly:
        ActiveSides.Add({ &BaseSide, &LeftPane, &LeftPaneCommands });
        break;
    case EGraphXPortDiffMode::NewFile:
        ActiveSides.Add({ &ResultSide, &ResultPane, &ResultPaneCommands });
        break;
    case EGraphXPortDiffMode::None:
    default:
        break;
    }

    if (ActiveSides.Num() == 0)
    {
        PanesContainer->SetContent(
            SNew(SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(LOCTEXT("SelectFile", "Select a file from the list."))
            ]);
        return;
    }

    TSharedRef<SSplitter> Splitter = SNew(SSplitter).Orientation(Orient_Horizontal);
    for (const FActiveSide& Active : ActiveSides)
    {
        Splitter->AddSlot()
        [
            BuildSideWidget(*Active.Side, *Active.Pane, *Active.Commands)
        ];
    }

    PanesContainer->SetContent(Splitter);
}

bool SGraphXPortDiffPanel::CompileMine()
{
    if (!ResultSide.Asset || !ResultSide.bEditable)
    {
        return false;
    }

    // Enums, structs and data tables have no Blueprint to compile.
    if (!ResultSide.Blueprint)
    {
        return true;
    }

    FKismetEditorUtilities::CompileBlueprint(ResultSide.Blueprint.Get());
    return ResultSide.Blueprint->Status != BS_Error;
}

bool SGraphXPortDiffPanel::SaveMinePackage()
{
    if (!ResultSide.Asset || !ResultSide.bEditable)
    {
        return false;
    }

    const bool bIsMap = ResultSide.Kind == EGraphXPortAssetKind::LevelScript;

    UPackage* Package = ResultSide.Asset->GetOutermost();
    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename,
        bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
    {
        return false;
    }

    FSavePackageArgs SaveArgs;
    // A map's top-level object is the world itself, which does not carry the
    // public/standalone flags an asset package's does.
    SaveArgs.TopLevelFlags = bIsMap ? RF_NoFlags : (RF_Public | RF_Standalone);
    return UPackage::SavePackage(Package, ResultSide.Asset.Get(), *Filename, SaveArgs);
}

bool SGraphXPortDiffPanel::StageResolvedFile()
{
    return Entry.IsValid() && FGraphXPortGitStatus::StageFile(RepoRoot, Entry->RepoRelativePath);
}

FReply SGraphXPortDiffPanel::OnSaveClicked()
{
    const bool bCompiled = CompileMine();
    const bool bSaved = SaveMinePackage();

    if (StatusText.IsValid())
    {
        if (!bSaved)
        {
            StatusText->SetText(LOCTEXT("SaveFail", "Save failed - see the Output Log."));
        }
        else
        {
            StatusText->SetText(bCompiled
                ? LOCTEXT("SaveOk", "Saved.")
                : LOCTEXT("SaveWithErrors", "Saved, but the Blueprint has compile errors."));
        }
    }

    return FReply::Handled();
}

FReply SGraphXPortDiffPanel::OnSaveAndResolveClicked()
{
    // Never stage a broken merge as "resolved" - a conflict marked resolved is easy to
    // stop looking at, so the compile failure has to stop the whole action here.
    if (!CompileMine())
    {
        if (StatusText.IsValid())
        {
            StatusText->SetText(LOCTEXT("CompileFail", "Compile failed - fix the errors before marking resolved. Nothing was saved or staged."));
        }
        return FReply::Handled();
    }

    if (!SaveMinePackage())
    {
        if (StatusText.IsValid())
        {
            StatusText->SetText(LOCTEXT("SaveFail", "Save failed - see the Output Log."));
        }
        return FReply::Handled();
    }

    const bool bStaged = StageResolvedFile();

    if (StatusText.IsValid())
    {
        StatusText->SetText(bStaged
            ? LOCTEXT("ResolvedOk", "Saved and marked resolved. The merged result is staged; commit it from your git tool.")
            : LOCTEXT("StageFail", "Saved, but staging failed - see the Output Log."));
    }

    if (bStaged)
    {
        OnFileResolved.ExecuteIfBound();
    }

    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
