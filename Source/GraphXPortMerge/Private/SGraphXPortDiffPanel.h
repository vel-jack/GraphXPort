#pragma once

#include "CoreMinimal.h"
#include "DiffResults.h"
#include "DiffUtils.h"
#include "GraphXPortAssetDiff.h"
#include "GraphXPortGitStatus.h"
#include "UObject/GCObject.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UPackage;
class FUICommandList;
class SBox;
class SGraphEditor;
class SMyBlueprint;
class SKismetInspector;
class FSCSDiff;
class IDetailsView;

/** Explicit destinations; navigation never depends on localized category labels. */
enum class EGraphXPortView : uint8
{
    MyBlueprint, Defaults, ClassSettings, Components, Graph, Entries
};

enum class EGraphXPortDiffMode : uint8
{
    None,
    /** Both-modified (git status U): base / mine (editable, real asset) / theirs. */
    Conflict3Way,
    /** Unstaged edit against HEAD: HEAD (reference) / working tree (editable, real asset). */
    ModifiedCompare,
    /** Deleted in the working tree: only the HEAD copy is available, read-only. */
    SingleReadOnly,
    /** Untracked or newly added: only the real, editable asset, nothing to compare against. */
    NewFile,
    /** Staged or committed change: two historical revisions, both read-only. */
    ReadOnlyCompare
};

/** What the Result file on disk currently matches, in a conflict. */
enum class EGraphXPortResultOrigin : uint8
{
    Unknown,
    /** Identical to git stage 2 - what git leaves in the working tree for a binary conflict. */
    Mine,
    /** Identical to git stage 3, after "Use Incoming as Result". */
    Incoming,
    /** Differs from both: the user has saved merge work into it. */
    Merged
};

/** A row in the differences tree: either a category (a graph, or Variables) or one change. */
struct FGraphXPortDiffTreeNode
{
    EGraphXPortView View = EGraphXPortView::MyBlueprint;
    FName Icon;
    FPropertySoftPath PropertyPath;
    bool bIsDifference = false;
    FText Label;
    FLinearColor Color = FLinearColor::White;
    bool bIsCategory = false;
    /** Rows for non-graph changes (variables, defaults, components) switch the panes to
     *  the member-list view instead of a graph. */
    bool bIsMemberRow = false;
    /** Graph this row belongs to; NAME_None for non-graph rows. */
    FName GraphName;
    /** Component whose values this row refers to; NAME_None means inspect the class defaults. */
    FName ComponentName;
    /** Leaf rows only: the nodes to focus on each side. */
    UEdGraphNode* BaseNode = nullptr;
    UEdGraphNode* SideNode = nullptr;
    bool bFromTheirs = false;
    bool bCanJump = false;
    /** Which section this row came from: a graph name, or Variables/Components/Defaults. */
    FText CategoryLabel;
    /** Member identity used to attach variable changes to the single variable row. */
    FName MemberName;
    /** The node to focus. Each pane resolves it in its own revision, so a row holds no
     *  pointer into one particular revision's graph. */
    FGuid NodeGuid;

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> Children;
};

/**
 * Shows the selected file's Blueprint graph(s) side by side using real, native
 * SGraphEditor panes bound to actually-loaded UBlueprint revisions - historical
 * revisions are loaded read-only via the engine's diff loader (see
 * FGraphXPortTempPackage). For unstaged changes the Result pane is the real, live /Game
 * asset, so normal Unreal graph editing (including copy/paste between panes) and a
 * normal package save are how conflicts actually get resolved. Staged and committed
 * changes compare two historical revisions and nothing is editable.
 *
 * Changed nodes are highlighted by SGraphEditor itself (via its DiffResults argument).
 * The differences tree - owned here but displayed by the merge window under the file
 * list - lists every graph and variable change and jumps the panes to each one.
 */
class SGraphXPortDiffPanel final : public SCompoundWidget, public FGCObject
{
public:
    SLATE_BEGIN_ARGS(SGraphXPortDiffPanel) {}
        /** Raised after a conflict is resolved and staged, so the file list can refresh. */
        SLATE_EVENT(FSimpleDelegate, OnFileResolved)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SGraphXPortDiffPanel() override;

    void SetFile(const FString& InRepoRoot, TSharedPtr<FGraphXPortGitFileEntry> InEntry);

    /** The differences tree, so the merge window can place it under the file list. */
    TSharedRef<SWidget> GetDifferencesTree() const;

    //~ FGCObject: the displayed revisions are raw UObject pointers with no other owner,
    // so without this GC can collect a revision out from under a live SGraphEditor.
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("SGraphXPortDiffPanel"); }

private:
    friend class FGraphXPortNavigationTest;
    struct FSideData
    {
        FText Label;
        /** The asset itself; for a level this is the world, not the script Blueprint. */
        TObjectPtr<UObject> Asset = nullptr;
        TObjectPtr<UBlueprint> Blueprint = nullptr;
        TObjectPtr<UPackage> Package = nullptr;
        EGraphXPortAssetKind Kind = EGraphXPortAssetKind::None;
        bool bEditable = false;
        bool bIsTemp = false;
        FString UnavailableReason;
        TSharedPtr<TArray<FDiffSingleResult>> DiffResults;
        /** Populated for entry-based kinds (enum, struct, data table). */
        TArray<FGraphXPortAssetEntry> Entries;
        TArray<FString> Columns;
        // Native widgets must be destroyed before their revision objects are released.
        TSharedPtr<SMyBlueprint> MyBlueprint;
        TSharedPtr<SKismetInspector> Inspector;
        TSharedPtr<FSCSDiff> Components;
        TSharedPtr<IDetailsView> Details;
    };

    /** Per-graph diff results, computed once per file so the tree can list every graph. */
    struct FGraphDiffSet
    {
        TSharedPtr<TArray<FDiffSingleResult>> MineDiffs;
        TSharedPtr<TArray<FDiffSingleResult>> TheirsDiffs;
        TSharedPtr<TArray<FDiffSingleResult>> ResultDiffs;
        TSharedPtr<TArray<FDiffSingleResult>> BaseDiffs;
    };

    void ReleaseLoadedRevisions();
    void RebuildSides();
    /** RefSpec is anything git accepts before ":path" - a commit, "HEAD", a conflict stage
     *  (":2"), or empty for the index. */
    void LoadTempSide(FSideData& Side, const FString& RefSpec, const FString& RepoRelativePath);
    void LoadRealSide(FSideData& Side, bool bEditable);
    void CollectGraphNames();
    void ComputeAllDiffs();
    void AppendVariableDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendDefaultsDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendClassSettingsDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendComponentDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendEntryDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendGraphPresenceDiffs(FName GraphName, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void BuildDiffTree();
    void FilterNavigation();
    FReply NavigateDifference(int32 Direction);
    FReply RefreshComparison();
    FReply ToggleOrientation();
    void EnsureInspector(FSideData& Side);
    void ReleaseSideWidgets();
    /** Assembles the Mine / Theirs sections from the gathered rows. */
    void PartitionRowsIntoTree(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& AllRows);
    void ApplySelectedGraphDiffResults();
    void RebuildPanes();

    /**
     * The side holding "my" changes for comparison purposes. In a conflict that is the
     * read-only stage-2 revision; otherwise there are no git stages and the working tree
     * itself is the only thing that changed.
     */
    const FSideData& LocalChangeSide() const
    {
        return Mode == EGraphXPortDiffMode::Conflict3Way ? MineSide : ResultSide;
    }

    /** Prefix for the local-change rows in the differences tree: MINE, WORKING, STAGED or COMMIT. */
    FText LocalChangeTag() const;

    UEdGraph* FindGraphForSide(const FSideData& Side, FName GraphName) const;
    UEdGraphNode* FindNodeForSide(const FSideData& Side, FName GraphName, const FGuid& NodeGuid) const;
    /** Queues a focus for the next tick. Jumping in the same frame the panes were rebuilt
     *  does nothing, because a freshly constructed SGraphEditor has no geometry to centre
     *  against yet. */
    void RequestJumpToNode(FName GraphName, const FGuid& NodeGuid);
    EActiveTimerReturnType HandlePendingJump(double InCurrentTime, float InDeltaTime);
    TSharedRef<SWidget> BuildSideWidget(FSideData& Side, TSharedPtr<SGraphEditor>& OutPane, TSharedPtr<FUICommandList>& OutCommands);
    TSharedRef<SWidget> BuildMembersWidget(FSideData& Side);
    TSharedRef<SWidget> BuildEntryListWidget(const FSideData& Side) const;
    TSharedRef<SWidget> BuildToolbarRow();
    TSharedRef<SWidget> BuildDiffTreeWidget();
    TSharedRef<ITableRow> GenerateTreeRow(TSharedPtr<FGraphXPortDiffTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void OnGetTreeChildren(TSharedPtr<FGraphXPortDiffTreeNode> Item, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutChildren);
    void OnTreeSelectionChanged(TSharedPtr<FGraphXPortDiffTreeNode> Item, ESelectInfo::Type SelectInfo);
    void ExpandAllTreeItems();
    FText GetCurrentGraphLabel() const;
    FReply OnSaveClicked();
    FReply OnSaveAndResolveClicked();
    FReply OnToggleLeftPaneClicked();
    FText GetLeftToggleLabel() const;
    /** Hashes the working file against stages 2 and 3 to see which side Result matches. */
    void RefreshResultOrigin();
    /** Replaces the Result file with Mine (stage 2) or Incoming (stage 3) and reloads it. */
    FReply OnUseSideAsResultClicked(bool bIncoming);
    bool CanUseSideAsResult(bool bIncoming) const;
    FText GetResultOriginText() const;
    FSlateColor GetResultOriginColor() const;
    /** Compiles the Mine blueprint; false if it finished with errors. */
    bool CompileMine();
    bool SaveMinePackage();
    bool StageResolvedFile();
    EVisibility GetConflictActionVisibility() const;
    EVisibility GetPlainSaveVisibility() const;

    static void CopySelectedNodes(const TSharedPtr<SGraphEditor>* Pane);
    void PasteNodesIntoMine();
    void DeleteSelectedMineNodes();
    bool CanPasteIntoMine() const;

    FString RepoRoot;
    TSharedPtr<FGraphXPortGitFileEntry> Entry;
    EGraphXPortDiffMode Mode = EGraphXPortDiffMode::None;

    // Base, Mine and Theirs are the three git stages, all read-only. Result is the real
    // working asset - the only editable side, and the only one ever saved.
    FSideData BaseSide;
    FSideData MineSide;
    FSideData TheirsSide;
    FSideData ResultSide;

    /** The left pane shows Base or Mine; Mine is what you copy from once Result is Incoming. */
    bool bLeftShowsMine = false;

    EGraphXPortResultOrigin ResultOrigin = EGraphXPortResultOrigin::Unknown;

    TArray<FName> GraphNames;
    FName SelectedGraphName;
    /** True while a non-graph difference is selected: panes show member lists, not graphs. */
    EGraphXPortView CurrentView = EGraphXPortView::MyBlueprint;
    FName InspectedMemberName;
    FPropertySoftPath InspectedProperty;
    bool bVerticalPanes = false;
    /** Object shown in the member view's details panel: a component, or the class defaults. */
    FName InspectedComponentName;
    TMap<FName, FGraphDiffSet> GraphDiffs;

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> DiffTreeRoots;
    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> VisibleTreeRoots;
    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> VisibleDifferences;
    TMap<const FGraphXPortDiffTreeNode*, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>> VisibleChildren;
    FString NavigationFilter;
    bool bChangesOnly = false;
    TSharedPtr<STreeView<TSharedPtr<FGraphXPortDiffTreeNode>>> DiffTreeView;
    TSharedPtr<SWidget> DiffTreeWidget;

    /** Left pane, showing whichever of Base/Mine the toggle selects. */
    TSharedPtr<SGraphEditor> LeftPane;
    TSharedPtr<SGraphEditor> ResultPane;
    TSharedPtr<SGraphEditor> TheirsPane;

    // SGraphEditor only holds a weak reference to appended command lists, so these must
    // be kept alive here or every binding silently stops working.
    TSharedPtr<FUICommandList> LeftPaneCommands;
    TSharedPtr<FUICommandList> ResultPaneCommands;
    TSharedPtr<FUICommandList> TheirsPaneCommands;

    FGuid PendingJumpNodeGuid;
    FName PendingJumpGraphName;
    bool bJumpTimerRegistered = false;

    TSharedPtr<SBox> PanesContainer;
    TSharedPtr<STextBlock> StatusText;
    FSimpleDelegate OnFileResolved;
};
