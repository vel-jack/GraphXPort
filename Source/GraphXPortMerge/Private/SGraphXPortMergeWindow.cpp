#include "SGraphXPortMergeWindow.h"

#include "SGraphXPortDiffPanel.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

void SGraphXPortMergeWindow::Construct(const FArguments& InArgs)
{
    const EGraphXPortGitAvailability Availability = FGraphXPortGitStatus::DetectAvailability(RepoRoot);

    if (Availability != EGraphXPortGitAvailability::Available)
    {
        ChildSlot
        [
            BuildUnavailableNotice(Availability)
        ];
        return;
    }

    // Built first so its differences tree can be placed under the file list.
    SAssignNew(DiffPanel, SGraphXPortDiffPanel)
        .OnFileResolved(FSimpleDelegate::CreateSP(this, &SGraphXPortMergeWindow::OnFileResolved));

    ChildSlot
    [
        SNew(SSplitter)
        .Orientation(Orient_Horizontal)
        + SSplitter::Slot()
        .Value(0.28f)
        [
            SNew(SSplitter)
            .Orientation(Orient_Vertical)
            + SSplitter::Slot()
            .Value(0.4f)
            [
                BuildFileListPanel()
            ]
            + SSplitter::Slot()
            .Value(0.6f)
            [
                DiffPanel->GetDifferencesTree()
            ]
        ]
        + SSplitter::Slot()
        .Value(0.72f)
        [
            DiffPanel.ToSharedRef()
        ]
    ];

    RefreshFileList();
}

TSharedRef<SWidget> SGraphXPortMergeWindow::BuildUnavailableNotice(EGraphXPortGitAvailability Availability) const
{
    const FText Message = Availability == EGraphXPortGitAvailability::GitNotInstalled
        ? LOCTEXT("GitNotInstalled", "Git was not found on this machine.\nInstall Git and reopen GraphXPort Merge.")
        : LOCTEXT("NotARepository", "This project is not inside a git working tree.\nGraphXPort Merge needs the project to be under git.");

    return SNew(SBox)
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(Message)
            .Justification(ETextJustify::Center)
        ];
}

TSharedRef<SWidget> SGraphXPortMergeWindow::BuildFileListPanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ChangedFiles", "Changed Files (unstaged)"))
                .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .HAlign(HAlign_Right)
            [
                SNew(SButton)
                .Text(LOCTEXT("Refresh", "Refresh"))
                .OnClicked(this, &SGraphXPortMergeWindow::OnRefreshClicked)
            ]
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(FileListView, SListView<FGraphXPortGitFileEntryPtr>)
            .ListItemsSource(&FileEntries)
            .OnGenerateRow(this, &SGraphXPortMergeWindow::GenerateFileRow)
            .OnSelectionChanged(this, &SGraphXPortMergeWindow::OnFileSelectionChanged)
            .SelectionMode(ESelectionMode::Single)
        ];
}

void SGraphXPortMergeWindow::RefreshFileList()
{
    TArray<FGraphXPortGitFileEntry> Entries;
    FGraphXPortGitStatus::ListUnstagedChanges(RepoRoot, Entries);

    FileEntries.Reset(Entries.Num());
    for (FGraphXPortGitFileEntry& Entry : Entries)
    {
        FileEntries.Add(MakeShared<FGraphXPortGitFileEntry>(MoveTemp(Entry)));
    }

    if (FileListView.IsValid())
    {
        FileListView->RequestListRefresh();
    }
}

TSharedRef<ITableRow> SGraphXPortMergeWindow::GenerateFileRow(FGraphXPortGitFileEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(STableRow<FGraphXPortGitFileEntryPtr>, OwnerTable)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(6.f, 2.f)
            [
                SNew(SBox)
                .WidthOverride(24.f)
                [
                    SNew(STextBlock)
                    .Text(StatusCodeToLabel(Entry->StatusCode))
                    .ColorAndOpacity(StatusCodeToColor(Entry->StatusCode))
                    .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
                ]
            ]
            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Entry->RepoRelativePath))
            ]
        ];
}

FReply SGraphXPortMergeWindow::OnRefreshClicked()
{
    RefreshFileList();
    return FReply::Handled();
}

void SGraphXPortMergeWindow::OnFileSelectionChanged(FGraphXPortGitFileEntryPtr SelectedEntry, ESelectInfo::Type SelectInfo)
{
    if (DiffPanel.IsValid())
    {
        DiffPanel->SetFile(RepoRoot, SelectedEntry);
    }
}

void SGraphXPortMergeWindow::OnFileResolved()
{
    // The resolved entry is gone from the unstaged list, so clear the panes before its
    // backing entry is destroyed.
    if (FileListView.IsValid())
    {
        FileListView->ClearSelection();
    }
    if (DiffPanel.IsValid())
    {
        DiffPanel->SetFile(RepoRoot, nullptr);
    }

    RefreshFileList();
}

FText SGraphXPortMergeWindow::StatusCodeToLabel(TCHAR StatusCode)
{
    return FText::FromString(FString::Chr(StatusCode));
}

FSlateColor SGraphXPortMergeWindow::StatusCodeToColor(TCHAR StatusCode)
{
    switch (StatusCode)
    {
    case TEXT('U'):
        return FSlateColor(FLinearColor(0.95f, 0.25f, 0.2f)); // conflicted
    case TEXT('A'):
    case TEXT('?'):
        return FSlateColor(FLinearColor(0.3f, 0.85f, 0.35f)); // new/untracked
    case TEXT('D'):
        return FSlateColor(FLinearColor(0.9f, 0.6f, 0.2f)); // deleted
    case TEXT('R'):
        return FSlateColor(FLinearColor(0.4f, 0.7f, 0.95f)); // renamed
    case TEXT('M'):
    default:
        return FSlateColor(FLinearColor(0.85f, 0.85f, 0.3f)); // modified
    }
}

#undef LOCTEXT_NAMESPACE
