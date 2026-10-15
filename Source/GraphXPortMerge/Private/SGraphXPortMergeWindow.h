#pragma once

#include "CoreMinimal.h"
#include "GraphXPortGitHistory.h"
#include "GraphXPortGitStatus.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

using FGraphXPortGitFileEntryPtr = TSharedPtr<FGraphXPortGitFileEntry>;

class SGraphXPortDiffPanel;

/** A row in the file list: a section header, a file, a commit, or the "load more" row. */
struct FGraphXPortFileListItem
{
    enum class EKind : uint8
    {
        Section,
        File,
        Commit,
        LoadMore
    };

    EKind Kind = EKind::Section;
    FText Label;
    FGraphXPortGitFileEntryPtr File;
    TSharedPtr<FGraphXPortGitCommit> Commit;
    TArray<TSharedPtr<FGraphXPortFileListItem>> Children;
};

using FGraphXPortFileListItemPtr = TSharedPtr<FGraphXPortFileListItem>;

/**
 * Root widget for the "GraphXPort Merge" tab: gates on git being installed and the
 * project being inside a git working tree, then lists unstaged changes, staged changes
 * and commit history on the left and shows the selected file's revisions side by side
 * on the right via SGraphXPortDiffPanel.
 */
class SGraphXPortMergeWindow final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SGraphXPortMergeWindow) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Tears down the displayed revisions and their panes. Must run before the loaded
     *  revisions are garbage collected, or the panes would be left dangling. */
    void ReleaseRevisions();

private:
    TSharedRef<SWidget> BuildUnavailableNotice(EGraphXPortGitAvailability Availability) const;
    TSharedRef<SWidget> BuildFileListPanel();

    /** Reloads the working-tree sections and the first page of history. */
    void RefreshFileList();
    void LoadMoreHistory();
    void UpdateSectionLabels();
    void FilterFiles();

    TSharedRef<ITableRow> GenerateFileRow(FGraphXPortFileListItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
    TSharedPtr<SWidget> BuildFileContextMenu();
    void OpenWorkingAsset(FGraphXPortGitFileEntryPtr Entry, bool bShowInBrowser);
    void OnGetFileChildren(FGraphXPortFileListItemPtr Item, TArray<FGraphXPortFileListItemPtr>& OutChildren);
    FReply OnRefreshClicked();
    void OnFileSelectionChanged(FGraphXPortFileListItemPtr SelectedItem, ESelectInfo::Type SelectInfo);
    void OnFileResolved();
    /** Clears the panes before the entries they point at are rebuilt. */
    void ClearSelectedFile();

    static FText StatusCodeToLabel(TCHAR StatusCode);
    static FSlateColor StatusCodeToColor(TCHAR StatusCode);
    static FText BuildCommitTooltip(const FGraphXPortGitCommit& Commit);

    FString RepoRoot;

    TArray<FGraphXPortFileListItemPtr> RootItems;
    TArray<FGraphXPortFileListItemPtr> VisibleRoots;
    TMap<const FGraphXPortFileListItem*, TArray<FGraphXPortFileListItemPtr>> VisibleFileChildren;
    FString FileFilter;
    FGraphXPortGitFileEntryPtr SelectedFile;
    FGraphXPortFileListItemPtr UnstagedSection;
    FGraphXPortFileListItemPtr StagedSection;
    FGraphXPortFileListItemPtr HistorySection;
    FGraphXPortFileListItemPtr LoadMoreItem;
    /** Commits already listed, i.e. the --skip for the next page. */
    int32 LoadedCommitCount = 0;
    bool bHistoryExhausted = false;

    TSharedPtr<STreeView<FGraphXPortFileListItemPtr>> FileTreeView;
    TSharedPtr<SGraphXPortDiffPanel> DiffPanel;
};
