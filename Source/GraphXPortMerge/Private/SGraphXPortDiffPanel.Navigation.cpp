#include "SGraphXPortDiffPanel.h"

#include "DetailsDiff.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GraphDiffControl.h"
#include "GraphEditor.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

void SGraphXPortDiffPanel::AppendClassSettingsDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const
{
    if (!BaseSide.Blueprint) return;

    FDetailsDiff BaseDetails(BaseSide.Blueprint, FDetailsDiff::FOnDisplayedPropertiesChanged());
    auto Compare = [&](const FSideData& Side, bool bTheirs)
    {
        if (!Side.Blueprint) return;
        FDetailsDiff SideDetails(Side.Blueprint, FDetailsDiff::FOnDisplayedPropertiesChanged());
        TArray<FSingleObjectDiffEntry> Differences;
        BaseDetails.DiffAgainst(SideDetails, Differences);
        for (const FSingleObjectDiffEntry& Difference : Differences)
        {
            auto Row = MakeShared<FGraphXPortDiffTreeNode>();
            Row->Label = FText::Format(LOCTEXT("ClassSettingChanged", "{0}: {1}"),
                bTheirs ? LOCTEXT("IncomingTag", "THEIRS") : LocalChangeTag(),
                FText::FromString(Difference.Identifier.ToDisplayName()));
            Row->Color = FLinearColor(0.9f, 0.75f, 0.25f);
            Row->PropertyPath = Difference.Identifier;
            Row->bFromTheirs = bTheirs;
            OutRows.Add(Row);
        }
    };
    Compare(LocalChangeSide(), false);
    if (Mode == EGraphXPortDiffMode::Conflict3Way) Compare(TheirsSide, true);
}

void SGraphXPortDiffPanel::BuildDiffTree()
{
    DiffTreeRoots.Reset();
    if (!Entry.IsValid() && !BaseSide.Asset && !ResultSide.Asset && !MineSide.Asset && !TheirsSide.Asset)
    {
        FilterNavigation();
        return;
    }

    const bool bBlueprint = BaseSide.Blueprint || MineSide.Blueprint || ResultSide.Blueprint || TheirsSide.Blueprint;
    if (!bBlueprint)
    {
        TArray<TSharedPtr<FGraphXPortDiffTreeNode>> Rows;
        AppendEntryDiffs(Rows);
        PartitionRowsIntoTree(Rows);
        return;
    }

    auto Section = [this](const FText& Label, EGraphXPortView View, FName Graph = NAME_None)
    {
        auto Root = MakeShared<FGraphXPortDiffTreeNode>();
        Root->Label = Label;
        Root->Color = FLinearColor(0.8f, 0.8f, 0.8f);
        Root->bIsCategory = true;
        Root->View = View;
        Root->GraphName = Graph;
        DiffTreeRoots.Add(Root);
        return Root;
    };

    auto Members = Section(LOCTEXT("NavMyBlueprint", "My Blueprint"), EGraphXPortView::MyBlueprint);
    AppendVariableDiffs(Members->Children);
    auto Defaults = Section(LOCTEXT("NavDefaults", "Defaults"), EGraphXPortView::Defaults);
    AppendDefaultsDiffs(Defaults->Children);
    auto Settings = Section(LOCTEXT("NavClassSettings", "Class Settings"), EGraphXPortView::ClassSettings);
    AppendClassSettingsDiffs(Settings->Children);
    auto Components = Section(LOCTEXT("NavComponents", "Components"), EGraphXPortView::Components);
    Components->Icon = "Kismet.Tabs.Components";
    AppendComponentDiffs(Components->Children);

    // Construction Script and Event Graph first, then other functions/macros/collapsed graphs.
    TArray<FName> OrderedGraphs = GraphNames;
    OrderedGraphs.Sort([](FName A, FName B)
    {
        auto Priority = [](FName Name)
        {
            if (Name == FName("UserConstructionScript")) return 0;
            if (Name == FName("EventGraph")) return 1;
            return 2;
        };
        return Priority(A) != Priority(B) ? Priority(A) < Priority(B) : A.LexicalLess(B);
    });
    for (FName GraphName : OrderedGraphs)
    {
        FText Label = FText::FromString(FName::NameToDisplayString(GraphName.ToString(), false));
        FName Icon("GraphEditor.Function_16x");
        for (const FSideData* Side : { &ResultSide, &MineSide, &TheirsSide, &BaseSide })
        {
            if (UEdGraph* Graph = FindGraphForSide(*Side, GraphName))
            {
                if (const UEdGraphSchema* Schema = Graph->GetSchema())
                {
                    FGraphDisplayInfo Info;
                    Schema->GetGraphDisplayInformation(*Graph, Info);
                    Label = Info.PlainName;
                }
                if (Side->Blueprint->UbergraphPages.Contains(Graph)) Icon = "GraphEditor.EventGraph_16x";
                else if (Side->Blueprint->MacroGraphs.Contains(Graph)) Icon = "GraphEditor.Macro_16x";
                break;
            }
        }
        auto Root = Section(Label, EGraphXPortView::Graph, GraphName);
        Root->Icon = Icon;
        AppendGraphPresenceDiffs(GraphName, Root->Children);

        if (const FGraphDiffSet* Set = GraphDiffs.Find(GraphName))
        {
            auto Append = [&](const TArray<FDiffSingleResult>& Diffs, bool bTheirs)
            {
                for (const FDiffSingleResult& Diff : Diffs)
                {
                    auto Row = MakeShared<FGraphXPortDiffTreeNode>();
                    Row->Label = FText::Format(LOCTEXT("NavGraphChange", "{0}: {1}"),
                        bTheirs ? LOCTEXT("IncomingTag", "THEIRS") : LocalChangeTag(), Diff.DisplayString);
                    Row->Color = Diff.GetDisplayColor();
                    Row->bFromTheirs = bTheirs;
                    const UEdGraphNode* Node = Diff.Node1 ? Diff.Node1 : Diff.Node2;
                    if (Node) Row->NodeGuid = Node->NodeGuid;
                    Row->bCanJump = Row->NodeGuid.IsValid();
                    Root->Children.Add(Row);
                }
            };
            Append(Mode == EGraphXPortDiffMode::Conflict3Way ? *Set->MineDiffs : *Set->ResultDiffs, false);
            if (Mode == EGraphXPortDiffMode::Conflict3Way) Append(*Set->TheirsDiffs, true);
        }
    }

    const bool bComparable = BaseSide.Blueprint && (LocalChangeSide().Blueprint || TheirsSide.Blueprint);
    for (const auto& Root : DiffTreeRoots)
    {
        for (const auto& Row : Root->Children)
        {
            Row->View = Root->View;
            Row->GraphName = Root->GraphName;
            Row->bIsDifference = true;
        }
        if (Root->Children.IsEmpty())
        {
            auto Empty = MakeShared<FGraphXPortDiffTreeNode>();
            Empty->View = Root->View;
            Empty->GraphName = Root->GraphName;
            Empty->Label = bComparable
                ? LOCTEXT("NavNoDiff", "No differences detected.")
                : LOCTEXT("NavNoBase", "No comparable revision.");
            Empty->Color = FLinearColor(0.5f, 0.5f, 0.5f);
            Root->Children.Add(Empty);
        }
        else Root->Color = FLinearColor(0.9f, 0.75f, 0.25f);
    }
    FilterNavigation();
}

void SGraphXPortDiffPanel::PartitionRowsIntoTree(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& Rows)
{
    DiffTreeRoots.Reset();
    auto Root = MakeShared<FGraphXPortDiffTreeNode>();
    Root->View = EGraphXPortView::Entries;
    Root->bIsCategory = true;
    const auto Kind = ResultSide.Asset ? ResultSide.Kind : BaseSide.Kind;
    Root->Label = FGraphXPortAssetDiff::DescribeEntries(Kind);
    Root->Color = FLinearColor(0.8f, 0.8f, 0.8f);
    for (const auto& Row : Rows)
    {
        Row->View = EGraphXPortView::Entries;
        Row->bIsDifference = true;
        Root->Children.Add(Row);
    }
    DiffTreeRoots.Add(Root);
    FilterNavigation();
}

void SGraphXPortDiffPanel::FilterNavigation()
{
    VisibleTreeRoots.Reset();
    VisibleChildren.Reset();
    VisibleDifferences.Reset();
    for (const auto& Root : DiffTreeRoots)
    {
        TArray<TSharedPtr<FGraphXPortDiffTreeNode>> Children;
        const bool bRootMatches = NavigationFilter.IsEmpty() || Root->Label.ToString().Contains(NavigationFilter);
        for (const auto& Child : Root->Children)
        {
            if (bChangesOnly && !Child->bIsDifference) continue;
            if (bRootMatches || Child->Label.ToString().Contains(NavigationFilter)) Children.Add(Child);
        }
        if ((bChangesOnly || !bRootMatches) && Children.IsEmpty()) continue;
        VisibleTreeRoots.Add(Root);
        for (const auto& Child : Children)
            if (Child->bIsDifference) VisibleDifferences.Add(Child);
        VisibleChildren.Add(Root.Get(), MoveTemp(Children));
    }
    if (DiffTreeView.IsValid())
    {
        DiffTreeView->RequestTreeRefresh();
        ExpandAllTreeItems();
    }
}

void SGraphXPortDiffPanel::ExpandAllTreeItems()
{
    if (DiffTreeView.IsValid())
        for (const auto& Root : VisibleTreeRoots) DiffTreeView->SetItemExpansion(Root, true);
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildDiffTreeWidget()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(6.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [ SNew(STextBlock).Text(LOCTEXT("ReviewHeading", "Blueprint Review")).Font(FAppStyle::GetFontStyle("BoldFont")) ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).Text(LOCTEXT("PrevChange", "Prev"))
                .ToolTipText(LOCTEXT("PrevChangeTip", "Focus the previous visible difference."))
                .IsEnabled_Lambda([this] { return !VisibleDifferences.IsEmpty(); })
                .OnClicked(this, &SGraphXPortDiffPanel::NavigateDifference, -1)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton).Text(LOCTEXT("NextChange", "Next"))
                .ToolTipText(LOCTEXT("NextChangeTip", "Focus the next visible difference."))
                .IsEnabled_Lambda([this] { return !VisibleDifferences.IsEmpty(); })
                .OnClicked(this, &SGraphXPortDiffPanel::NavigateDifference, 1)
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(6.f, 2.f)
        [
            SNew(SSearchBox).HintText(LOCTEXT("SearchReview", "Search graphs and changes"))
            .OnTextChanged_Lambda([this](const FText& Text) { NavigationFilter = Text.ToString(); FilterNavigation(); })
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(6.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([this] { return bChangesOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bChangesOnly = State == ECheckBoxState::Checked; FilterNavigation(); })
                [ SNew(STextBlock).Text(LOCTEXT("ChangesOnly", "Changes only")) ]
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(STextBlock).Text_Lambda([this] {
                    return FText::Format(LOCTEXT("VisibleCount", "{0} changes"), FText::AsNumber(VisibleDifferences.Num()));
                })
            ]
        ]
        + SVerticalBox::Slot().FillHeight(1.f)
        [
            SAssignNew(DiffTreeView, STreeView<TSharedPtr<FGraphXPortDiffTreeNode>>)
            .TreeItemsSource(&VisibleTreeRoots)
            .OnGenerateRow(this, &SGraphXPortDiffPanel::GenerateTreeRow)
            .OnGetChildren(this, &SGraphXPortDiffPanel::OnGetTreeChildren)
            .OnSelectionChanged(this, &SGraphXPortDiffPanel::OnTreeSelectionChanged)
            .SelectionMode(ESelectionMode::Single)
        ];
}

TSharedRef<ITableRow> SGraphXPortDiffPanel::GenerateTreeRow(TSharedPtr<FGraphXPortDiffTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(STableRow<TSharedPtr<FGraphXPortDiffTreeNode>>, OwnerTable)
        .Padding(FMargin(2.f, 3.f))
        .ToolTipText(Item->Label)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SImage)
                .Visibility(Item->Icon.IsNone() ? EVisibility::Collapsed : EVisibility::Visible)
                .Image(Item->Icon.IsNone() ? nullptr : FAppStyle::GetBrush(Item->Icon))
            ]
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(Item->Label)
                .ColorAndOpacity(Item->Color)
                .Font(FAppStyle::GetFontStyle(Item->bIsCategory ? "BoldFont" : "NormalFont"))
            ]
        ];
}

void SGraphXPortDiffPanel::OnGetTreeChildren(TSharedPtr<FGraphXPortDiffTreeNode> Item, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutChildren)
{
    if (const auto* Children = VisibleChildren.Find(Item.Get())) OutChildren = *Children;
}

FReply SGraphXPortDiffPanel::NavigateDifference(int32 Direction)
{
    if (!DiffTreeView.IsValid() || VisibleDifferences.IsEmpty()) return FReply::Handled();
    const auto Selected = DiffTreeView->GetSelectedItems();
    int32 Index = Selected.IsEmpty() ? INDEX_NONE : VisibleDifferences.IndexOfByKey(Selected[0]);
    if (Index == INDEX_NONE) Index = Direction > 0 ? 0 : VisibleDifferences.Num() - 1;
    else Index = (Index + Direction + VisibleDifferences.Num()) % VisibleDifferences.Num();
    DiffTreeView->SetSelection(VisibleDifferences[Index]);
    DiffTreeView->RequestScrollIntoView(VisibleDifferences[Index]);
    return FReply::Handled();
}

void SGraphXPortDiffPanel::OnTreeSelectionChanged(TSharedPtr<FGraphXPortDiffTreeNode> Item, ESelectInfo::Type)
{
    if (!Item.IsValid()) return;
    CurrentView = Item->View;
    InspectedComponentName = Item->ComponentName;
    InspectedMemberName = Item->MemberName;
    InspectedProperty = Item->PropertyPath;
    if (!Item->GraphName.IsNone()) SelectedGraphName = Item->GraphName;
    if (Item->bIsDifference && !Item->bFromTheirs && Mode == EGraphXPortDiffMode::Conflict3Way)
        bLeftShowsMine = true;
    ApplySelectedGraphDiffResults();
    RebuildPanes();
    if (Item->bCanJump) RequestJumpToNode(Item->GraphName, Item->NodeGuid);
}

FReply SGraphXPortDiffPanel::RefreshComparison()
{
    const FName PreviousGraph = SelectedGraphName;
    CollectGraphNames();
    if (GraphNames.Contains(PreviousGraph)) SelectedGraphName = PreviousGraph;
    ComputeAllDiffs();
    BuildDiffTree();
    ApplySelectedGraphDiffResults();
    RebuildPanes();
    return FReply::Handled();
}

FReply SGraphXPortDiffPanel::ToggleOrientation()
{
    bVerticalPanes = !bVerticalPanes;
    RebuildPanes();
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
