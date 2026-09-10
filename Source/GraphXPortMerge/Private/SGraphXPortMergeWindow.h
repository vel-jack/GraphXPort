#pragma once

#include "CoreMinimal.h"
#include "GraphXPortGitStatus.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

using FGraphXPortGitFileEntryPtr = TSharedPtr<FGraphXPortGitFileEntry>;

class SGraphXPortDiffPanel;

/**
 * Root widget for the "GraphXPort Merge" tab: gates on git being installed and the
 * project being inside a git working tree, then lists unstaged working-tree changes
 * on the left and shows the selected file's Blueprint graph(s) side by side on the
 * right via SGraphXPortDiffPanel.
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

    void RefreshFileList();
    TSharedRef<ITableRow> GenerateFileRow(FGraphXPortGitFileEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable);
    FReply OnRefreshClicked();
    void OnFileSelectionChanged(FGraphXPortGitFileEntryPtr SelectedEntry, ESelectInfo::Type SelectInfo);
    void OnFileResolved();

    static FText StatusCodeToLabel(TCHAR StatusCode);
    static FSlateColor StatusCodeToColor(TCHAR StatusCode);

    FString RepoRoot;
    TArray<FGraphXPortGitFileEntryPtr> FileEntries;
    TSharedPtr<SListView<FGraphXPortGitFileEntryPtr>> FileListView;
    TSharedPtr<SGraphXPortDiffPanel> DiffPanel;
};
