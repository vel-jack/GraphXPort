#pragma once

#include "CoreMinimal.h"
#include "DiffResults.h"
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
    NewFile
};

/** A row in the differences tree: either a category (a graph, or Variables) or one change. */
struct FGraphXPortDiffTreeNode
{
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
    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> Children;
};

/**
 * Shows the selected file's Blueprint graph(s) side by side using real, native
 * SGraphEditor panes bound to actually-loaded UBlueprint revisions - historical
 * revisions are loaded read-only via the engine's diff loader (see
 * FGraphXPortTempPackage); the "Mine" pane is always the real, live /Game asset so
 * normal Unreal graph editing (including copy/paste between panes) and a normal
 * package save are how conflicts actually get resolved.
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
    struct FSideData
    {
        FText Label;
        TObjectPtr<UBlueprint> Blueprint = nullptr;
        TObjectPtr<UPackage> Package = nullptr;
        bool bEditable = false;
        bool bIsTemp = false;
        FString UnavailableReason;
        TSharedPtr<TArray<FDiffSingleResult>> DiffResults;
    };

    /** Per-graph diff results, computed once per file so the tree can list every graph. */
    struct FGraphDiffSet
    {
        TSharedPtr<TArray<FDiffSingleResult>> MineDiffs;
        TSharedPtr<TArray<FDiffSingleResult>> TheirsDiffs;
        TSharedPtr<TArray<FDiffSingleResult>> BaseDiffs;
    };

    void ReleaseLoadedRevisions();
    void RebuildSides();
    void LoadTempSide(FSideData& Side, const FString& RefSpec);
    void LoadRealSide(FSideData& Side, bool bEditable);
    void CollectGraphNames();
    void ComputeAllDiffs();
    void AppendVariableDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendDefaultsDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendComponentDiffs(TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void AppendGraphPresenceDiffs(FName GraphName, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutRows) const;
    void BuildDiffTree();
    void ApplySelectedGraphDiffResults();
    void RebuildPanes();

    UEdGraph* FindGraphForSide(const FSideData& Side, FName GraphName) const;
    TSharedRef<SWidget> BuildSideWidget(const FSideData& Side, TSharedPtr<SGraphEditor>& OutPane, TSharedPtr<FUICommandList>& OutCommands);
    TSharedRef<SWidget> BuildMembersWidget(const FSideData& Side) const;
    TSharedRef<SWidget> BuildToolbarRow();
    TSharedRef<SWidget> BuildDiffTreeWidget();
    TSharedRef<ITableRow> GenerateTreeRow(TSharedPtr<FGraphXPortDiffTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void OnGetTreeChildren(TSharedPtr<FGraphXPortDiffTreeNode> Item, TArray<TSharedPtr<FGraphXPortDiffTreeNode>>& OutChildren);
    void OnTreeSelectionChanged(TSharedPtr<FGraphXPortDiffTreeNode> Item, ESelectInfo::Type SelectInfo);
    void ExpandAllTreeItems();
    FText GetCurrentGraphLabel() const;
    FReply OnSaveClicked();
    FReply OnSaveAndResolveClicked();
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

    FSideData BaseSide;
    FSideData MineSide;
    FSideData TheirsSide;

    TArray<FName> GraphNames;
    FName SelectedGraphName;
    /** True while a non-graph difference is selected: panes show member lists, not graphs. */
    bool bShowMembers = false;
    /** Object shown in the member view's details panel: a component, or the class defaults. */
    FName InspectedComponentName;
    TMap<FName, FGraphDiffSet> GraphDiffs;

    TArray<TSharedPtr<FGraphXPortDiffTreeNode>> DiffTreeRoots;
    TSharedPtr<STreeView<TSharedPtr<FGraphXPortDiffTreeNode>>> DiffTreeView;
    TSharedPtr<SWidget> DiffTreeWidget;

    TSharedPtr<SGraphEditor> BasePane;
    TSharedPtr<SGraphEditor> MinePane;
    TSharedPtr<SGraphEditor> TheirsPane;

    // SGraphEditor only holds a weak reference to appended command lists, so these must
    // be kept alive here or every binding silently stops working.
    TSharedPtr<FUICommandList> BasePaneCommands;
    TSharedPtr<FUICommandList> MinePaneCommands;
    TSharedPtr<FUICommandList> TheirsPaneCommands;

    TSharedPtr<SBox> PanesContainer;
    TSharedPtr<STextBlock> StatusText;
    FSimpleDelegate OnFileResolved;
};
