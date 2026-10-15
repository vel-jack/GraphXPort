#include "SGraphXPortMergeWindow.h"

#include "SGraphXPortDiffPanel.h"
#include "GraphXPortTempPackage.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

namespace GraphXPortMergeWindowPrivate
{
    /** Commits fetched per page; more are loaded on request so a long history stays cheap. */
    static constexpr int32 HistoryPageSize = 100;

    static const FLinearColor SubduedColor(0.55f, 0.55f, 0.55f);
    static const FLinearColor HashColor(0.95f, 0.75f, 0.35f);

    static FGraphXPortFileListItemPtr MakeSection()
    {
        FGraphXPortFileListItemPtr Section = MakeShared<FGraphXPortFileListItem>();
        Section->Kind = FGraphXPortFileListItem::EKind::Section;
        return Section;
    }

    static FGraphXPortFileListItemPtr MakeFileItem(FGraphXPortGitFileEntry&& Entry)
    {
        FGraphXPortFileListItemPtr Item = MakeShared<FGraphXPortFileListItem>();
        Item->Kind = FGraphXPortFileListItem::EKind::File;
        Item->File = MakeShared<FGraphXPortGitFileEntry>(MoveTemp(Entry));
        return Item;
    }
}

void SGraphXPortMergeWindow::Construct(const FArguments& InArgs)
{
    using namespace GraphXPortMergeWindowPrivate;

    const EGraphXPortGitAvailability Availability = FGraphXPortGitStatus::DetectAvailability(RepoRoot);

    if (Availability != EGraphXPortGitAvailability::Available)
    {
        ChildSlot
        [
            BuildUnavailableNotice(Availability)
        ];
        return;
    }

    UnstagedSection = MakeSection();
    StagedSection = MakeSection();
    HistorySection = MakeSection();
    RootItems = { UnstagedSection, StagedSection, HistorySection };

    LoadMoreItem = MakeShared<FGraphXPortFileListItem>();
    LoadMoreItem->Kind = FGraphXPortFileListItem::EKind::LoadMore;

    // Built first so its differences tree can be placed under the file list.
    SAssignNew(DiffPanel, SGraphXPortDiffPanel)
        .OnFileResolved(FSimpleDelegate::CreateSP(this, &SGraphXPortMergeWindow::OnFileResolved));

    ChildSlot
    [
        SNew(SSplitter)
        .Orientation(Orient_Horizontal)
        + SSplitter::Slot()
        .Value(0.23f)
        [
            SNew(SSplitter)
            .Orientation(Orient_Vertical)
            + SSplitter::Slot()
            .Value(0.35f)
            [
                BuildFileListPanel()
            ]
            + SSplitter::Slot()
            .Value(0.65f)
            [
                DiffPanel->GetDifferencesTree()
            ]
        ]
        + SSplitter::Slot()
        .Value(0.77f)
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
                .Text(LOCTEXT("Files", "Files"))
                .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .HAlign(HAlign_Right)
            [
                SNew(SButton)
                .Text(LOCTEXT("Refresh", "Refresh"))
                .ToolTipText(LOCTEXT("RefreshTip", "Re-read git status and reload the latest history."))
                .OnClicked(this, &SGraphXPortMergeWindow::OnRefreshClicked)
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(6.f, 2.f, 6.f, 6.f)
        [
            SNew(SSearchBox).HintText(LOCTEXT("SearchFiles", "Search assets or commits"))
            .OnTextChanged_Lambda([this](const FText& Text) { FileFilter = Text.ToString(); FilterFiles(); })
        ]
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(FileTreeView, STreeView<FGraphXPortFileListItemPtr>)
            .TreeItemsSource(&VisibleRoots)
            .OnGenerateRow(this, &SGraphXPortMergeWindow::GenerateFileRow)
            .OnGetChildren(this, &SGraphXPortMergeWindow::OnGetFileChildren)
            .OnSelectionChanged(this, &SGraphXPortMergeWindow::OnFileSelectionChanged)
            .OnContextMenuOpening(this, &SGraphXPortMergeWindow::BuildFileContextMenu)
            .OnMouseButtonClick_Lambda([this](FGraphXPortFileListItemPtr Item)
            {
                // A click on a commit opens it to show its assets. Handled per click rather
                // than on selection change, so clicking the same commit again closes it.
                if (Item.IsValid() && Item->Kind == FGraphXPortFileListItem::EKind::Commit)
                {
                    FileTreeView->SetItemExpansion(Item, !FileTreeView->IsItemExpanded(Item));
                }
            })
            .SelectionMode(ESelectionMode::Single)
        ];
}

TSharedPtr<SWidget> SGraphXPortMergeWindow::BuildFileContextMenu()
{
    if (!FileTreeView.IsValid()) return nullptr;
    const TArray<FGraphXPortFileListItemPtr> Selection = FileTreeView->GetSelectedItems();
    if (Selection.Num() != 1 || !Selection[0].IsValid() || !Selection[0]->File.IsValid()) return nullptr;

    const FGraphXPortGitFileEntryPtr Entry = Selection[0]->File;
    const FString FilePath = FPaths::ConvertRelativePathToFull(RepoRoot / Entry->RepoRelativePath);
    if (!FPaths::IsUnderDirectory(FilePath, RepoRoot)) return nullptr;

    const FCanExecuteAction FileExists = FCanExecuteAction::CreateLambda([FilePath]()
    {
        return IFileManager::Get().FileExists(*FilePath);
    });
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(LOCTEXT("ShowInContentBrowser", "Show in Content Browser"),
        LOCTEXT("ShowInContentBrowserTip", "Locate the current working-tree asset in the Content Browser."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SGraphXPortMergeWindow::OpenWorkingAsset, Entry, true), FileExists));
    Menu.AddMenuEntry(LOCTEXT("ShowInExplorer", "Show in Explorer"),
        LOCTEXT("ShowInExplorerTip", "Locate the current working-tree file on disk."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([FilePath]() { FPlatformProcess::ExploreFolder(*FilePath); }), FileExists));
    Menu.AddMenuEntry(LOCTEXT("OpenOriginalAsset", "Open Original Asset"),
        LOCTEXT("OpenOriginalAssetTip", "Open the current working-tree asset in its Unreal Editor."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SGraphXPortMergeWindow::OpenWorkingAsset, Entry, false), FileExists));
    return Menu.MakeWidget();
}

void SGraphXPortMergeWindow::OpenWorkingAsset(FGraphXPortGitFileEntryPtr Entry, bool bShowInBrowser)
{
    FGraphXPortLoadedAsset Loaded;
    FString Error;
    if (!FGraphXPortTempPackage::ResolveRealAsset(RepoRoot, Entry->RepoRelativePath, Loaded, Error))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
        return;
    }

    if (bShowInBrowser)
    {
        GEditor->SyncBrowserToObjects(TArray<UObject*>{ Loaded.Asset });
    }
    else
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Loaded.Asset);
    }
}

void SGraphXPortMergeWindow::RefreshFileList()
{
    using namespace GraphXPortMergeWindowPrivate;

    // The diff panel keeps its own reference to the entry it shows, so rebuilding the
    // list does not disturb what is on screen.
    TArray<FGraphXPortGitFileEntry> Unstaged;
    TArray<FGraphXPortGitFileEntry> Staged;
    FGraphXPortGitStatus::ListChanges(RepoRoot, Unstaged, Staged);

    UnstagedSection->Children.Reset(Unstaged.Num());
    for (FGraphXPortGitFileEntry& Entry : Unstaged)
    {
        UnstagedSection->Children.Add(MakeFileItem(MoveTemp(Entry)));
    }

    StagedSection->Children.Reset(Staged.Num());
    for (FGraphXPortGitFileEntry& Entry : Staged)
    {
        StagedSection->Children.Add(MakeFileItem(MoveTemp(Entry)));
    }

    HistorySection->Children.Reset();
    LoadedCommitCount = 0;
    bHistoryExhausted = false;
    LoadMoreHistory();

    if (FileTreeView.IsValid())
    {
        for (const FGraphXPortFileListItemPtr& Root : RootItems)
        {
            FileTreeView->SetItemExpansion(Root, true);
        }
        FileTreeView->RequestTreeRefresh();
    }
}

void SGraphXPortMergeWindow::LoadMoreHistory()
{
    using namespace GraphXPortMergeWindowPrivate;

    HistorySection->Children.Remove(LoadMoreItem);

    TArray<FGraphXPortGitCommit> Commits;
    FGraphXPortGitHistory::ListCommits(RepoRoot, LoadedCommitCount, HistoryPageSize, Commits);

    for (FGraphXPortGitCommit& Commit : Commits)
    {
        FGraphXPortFileListItemPtr CommitItem = MakeShared<FGraphXPortFileListItem>();
        CommitItem->Kind = FGraphXPortFileListItem::EKind::Commit;
        for (FGraphXPortGitFileEntry& File : Commit.Files)
        {
            CommitItem->Children.Add(MakeFileItem(MoveTemp(File)));
        }
        Commit.Files.Reset();
        CommitItem->Commit = MakeShared<FGraphXPortGitCommit>(MoveTemp(Commit));
        HistorySection->Children.Add(CommitItem);
    }

    LoadedCommitCount += Commits.Num();
    bHistoryExhausted = Commits.Num() < HistoryPageSize;
    if (!bHistoryExhausted)
    {
        HistorySection->Children.Add(LoadMoreItem);
    }

    UpdateSectionLabels();
    FilterFiles();

    if (FileTreeView.IsValid())
    {
        FileTreeView->RequestTreeRefresh();
    }
}

void SGraphXPortMergeWindow::UpdateSectionLabels()
{
    UnstagedSection->Label = FText::Format(LOCTEXT("UnstagedSection", "Unstaged Changes ({0})"),
        FText::AsNumber(UnstagedSection->Children.Num()));
    StagedSection->Label = FText::Format(LOCTEXT("StagedSection", "Staged Changes ({0})"),
        FText::AsNumber(StagedSection->Children.Num()));
    HistorySection->Label = bHistoryExhausted
        ? FText::Format(LOCTEXT("HistorySectionAll", "History ({0})"), FText::AsNumber(LoadedCommitCount))
        : FText::Format(LOCTEXT("HistorySectionSome", "History (latest {0})"), FText::AsNumber(LoadedCommitCount));
}

TSharedRef<ITableRow> SGraphXPortMergeWindow::GenerateFileRow(FGraphXPortFileListItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    using namespace GraphXPortMergeWindowPrivate;

    TSharedPtr<SWidget> Content;

    switch (Item->Kind)
    {
    case FGraphXPortFileListItem::EKind::Section:
    {
        // Bound rather than copied so the count updates when more history is loaded.
        Content = SNew(STextBlock)
            .Text_Lambda([Item]() { return Item->Label; })
            .Font(FAppStyle::Get().GetFontStyle("BoldFont"));
        break;
    }

    case FGraphXPortFileListItem::EKind::Commit:
    {
        const FGraphXPortGitCommit& Commit = *Item->Commit;
        Content = SNew(SHorizontalBox)
            .ToolTipText(BuildCommitTooltip(Commit))
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 1.f, 8.f, 1.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Commit.ShortHash))
                .ColorAndOpacity(FSlateColor(HashColor))
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            .Padding(0.f, 1.f, 8.f, 1.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Commit.Subject))
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 1.f, 6.f, 1.f)
            [
                SNew(STextBlock)
                .Text(FText::Format(LOCTEXT("CommitMeta", "{0}  {1}"),
                    FText::FromString(Commit.Author), FText::FromString(Commit.Date)))
                .ColorAndOpacity(FSlateColor(SubduedColor))
            ];
        break;
    }

    case FGraphXPortFileListItem::EKind::LoadMore:
    {
        Content = SNew(SBox)
            .HAlign(HAlign_Left)
            .Padding(2.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("LoadMore", "Load more commits"))
                .OnClicked_Lambda([this]()
                {
                    LoadMoreHistory();
                    return FReply::Handled();
                })
            ];
        break;
    }

    case FGraphXPortFileListItem::EKind::File:
    default:
    {
        const FGraphXPortGitFileEntry& Entry = *Item->File;
        const FText Tooltip = Entry.OriginalPath.IsEmpty()
            ? FText::FromString(Entry.RepoRelativePath)
            : FText::Format(LOCTEXT("RenamedTip", "{0}\nrenamed from {1}"),
                FText::FromString(Entry.RepoRelativePath), FText::FromString(Entry.OriginalPath));

        Content = SNew(SHorizontalBox)
            .ToolTipText(Tooltip)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(2.f, 1.f)
            [
                SNew(SBox)
                .WidthOverride(22.f)
                [
                    SNew(STextBlock)
                    .Text(StatusCodeToLabel(Entry.StatusCode))
                    .ColorAndOpacity(StatusCodeToColor(Entry.StatusCode))
                    .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
                ]
            ]
            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Entry.RepoRelativePath))
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            ];
        break;
    }
    }

    return SNew(STableRow<FGraphXPortFileListItemPtr>, OwnerTable)
        [
            Content.ToSharedRef()
        ];
}

void SGraphXPortMergeWindow::OnGetFileChildren(FGraphXPortFileListItemPtr Item, TArray<FGraphXPortFileListItemPtr>& OutChildren)
{
    if (Item.IsValid())
    {
        if (const auto* Children = VisibleFileChildren.Find(Item.Get())) OutChildren = *Children;
    }
}

FReply SGraphXPortMergeWindow::OnRefreshClicked()
{
    const FGraphXPortGitFileEntryPtr Previous = SelectedFile;
    RefreshFileList();
    if (Previous.IsValid())
    {
        // Restore the exact comparison, not another entry for the same path in the index.
        TFunction<FGraphXPortFileListItemPtr(const TArray<FGraphXPortFileListItemPtr>&)> Find;
        Find = [&](const TArray<FGraphXPortFileListItemPtr>& Items) -> FGraphXPortFileListItemPtr
        {
            for (const auto& Item : Items)
            {
                if (Item->File && Item->File->RepoRelativePath == Previous->RepoRelativePath &&
                    Item->File->Source == Previous->Source && Item->File->CommitHash == Previous->CommitHash) return Item;
                if (auto Match = Find(Item->Children)) return Match;
            }
            return nullptr;
        };
        if (auto Match = Find(RootItems)) FileTreeView->SetSelection(Match);
        else if (Previous->Source != EGraphXPortChangeSource::Commit) ClearSelectedFile();
    }
    return FReply::Handled();
}

void SGraphXPortMergeWindow::OnFileSelectionChanged(FGraphXPortFileListItemPtr SelectedItem, ESelectInfo::Type SelectInfo)
{
    // Sections and commits are not files; the panes keep showing the last file selected.
    if (SelectedItem.IsValid() && SelectedItem->Kind == FGraphXPortFileListItem::EKind::File && DiffPanel.IsValid())
    {
        SelectedFile = SelectedItem->File;
        DiffPanel->SetFile(RepoRoot, SelectedFile);
    }
}

void SGraphXPortMergeWindow::ClearSelectedFile()
{
    SelectedFile.Reset();
    if (FileTreeView.IsValid())
    {
        FileTreeView->ClearSelection();
    }
    if (DiffPanel.IsValid())
    {
        DiffPanel->SetFile(RepoRoot, nullptr);
    }
}

void SGraphXPortMergeWindow::ReleaseRevisions()
{
    ClearSelectedFile();
}

void SGraphXPortMergeWindow::OnFileResolved()
{
    // The resolved file moves from Unstaged to Staged, so what the panes show is stale.
    ClearSelectedFile();
    RefreshFileList();
}

FText SGraphXPortMergeWindow::BuildCommitTooltip(const FGraphXPortGitCommit& Commit)
{
    FString Text = FString::Printf(TEXT("%s  %s\n%s\n%s"),
        *Commit.ShortHash, *Commit.Author, *Commit.Date, *Commit.Subject);
    if (!Commit.Body.IsEmpty())
    {
        Text += TEXT("\n\n") + Commit.Body;
    }
    return FText::FromString(Text);
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
    case TEXT('C'):
        return FSlateColor(FLinearColor(0.4f, 0.7f, 0.95f)); // renamed/copied
    case TEXT('M'):
    default:
        return FSlateColor(FLinearColor(0.85f, 0.85f, 0.3f)); // modified
    }
}


void SGraphXPortMergeWindow::FilterFiles()
{
    VisibleRoots.Reset();
    VisibleFileChildren.Reset();
    TFunction<bool(const FGraphXPortFileListItemPtr&, bool)> Filter;
    Filter = [&](const FGraphXPortFileListItemPtr& Item, bool bParentMatches)
    {
        FString SearchText = Item->Label.ToString();
        if (Item->File) SearchText = Item->File->RepoRelativePath;
        if (Item->Commit) SearchText = BuildCommitTooltip(*Item->Commit).ToString();
        const bool bMatches = bParentMatches || FileFilter.IsEmpty() || SearchText.Contains(FileFilter);
        TArray<FGraphXPortFileListItemPtr> Children;
        for (const auto& Child : Item->Children)
            if (Filter(Child, bMatches)) Children.Add(Child);
        const bool bVisible = bMatches || !Children.IsEmpty() ||
            Item->Kind == FGraphXPortFileListItem::EKind::LoadMore;
        VisibleFileChildren.Add(Item.Get(), MoveTemp(Children));
        if (bVisible && FileTreeView.IsValid() && !FileFilter.IsEmpty())
            FileTreeView->SetItemExpansion(Item, true);
        return bVisible;
    };
    for (const auto& Root : RootItems)
        if (Filter(Root, false)) VisibleRoots.Add(Root);
    if (FileTreeView.IsValid()) FileTreeView->RequestTreeRefresh();
}

#undef LOCTEXT_NAMESPACE
