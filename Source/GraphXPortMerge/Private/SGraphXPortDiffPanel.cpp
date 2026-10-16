#include "SGraphXPortDiffPanel.h"

#include "DiffUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
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
#include "GraphXPortGitProcess.h"
#include "GraphXPortTempPackage.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailsView.h"
#include "SKismetInspector.h"
#include "SMyBlueprint.h"
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
#include "Brushes/SlateDynamicImageBrush.h"
#include "Engine/Texture.h"
#include "Styling/SlateIconFinder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Views/SListView.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
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
    static const FLinearColor IndexColor(0.5f, 0.5f, 0.5f);
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
            SNew(SScrollBox).Orientation(Orient_Horizontal)
            + SScrollBox::Slot() [ BuildToolbarRow() ]
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
    // Anything that is not a package is shown as a plain text diff: reset the asset
    // state as for "no selection", then put the diff where the graph panes would be.
    if (InEntry.IsValid() && !FPackageName::IsPackageExtension(*FPaths::GetExtension(InEntry->RepoRelativePath)))
    {
        SetFile(InRepoRoot, nullptr);
        RepoRoot = InRepoRoot;
        PanesContainer->SetContent(BuildTextDiffWidget(*InEntry));
        return;
    }

    ReleaseLoadedRevisions();

    RepoRoot = InRepoRoot;
    Entry = InEntry;

    RebuildSides();
    RefreshResultOrigin();
    // When Result already holds Incoming, Mine is the side changes get copied from.
    bLeftShowsMine = ResultOrigin == EGraphXPortResultOrigin::Incoming;
    CollectGraphNames();
    ComputeAllDiffs();
    BuildDiffTree();
    if (DiffTreeView.IsValid() && !VisibleTreeRoots.IsEmpty())
    {
        DiffTreeView->SetSelection(VisibleTreeRoots[0]);
    }
    ApplySelectedGraphDiffResults();
    RebuildPanes();

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::GetEmpty());
    }
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildTextDiffWidget(const FGraphXPortGitFileEntry& TextEntry)
{
    const FString Paths = TextEntry.OriginalPath.IsEmpty()
        ? FString::Printf(TEXT("\"%s\""), *TextEntry.RepoRelativePath)
        : FString::Printf(TEXT("\"%s\" \"%s\""), *TextEntry.OriginalPath, *TextEntry.RepoRelativePath);
    // Full context, so the one hunk is the whole file and both sides can be rebuilt from it.
    const FString Git = TEXT("-c core.quotePath=false --no-pager ");
    const FString Diff = TEXT("diff --no-color --no-ext-diff -U999999 -M ");

    FString Args;
    FText LeftLabel = LOCTEXT("TextHead", "HEAD");
    FText RightLabel = LOCTEXT("TextWorking", "Working tree");
    switch (TextEntry.Source)
    {
    case EGraphXPortChangeSource::Staged:
        Args = Git + Diff + TEXT("--cached -- ") + Paths;
        RightLabel = LOCTEXT("TextStaged", "Staged (index)");
        break;
    case EGraphXPortChangeSource::Commit:
        Args = Git + Diff + FString::Printf(TEXT("%s^ %s -- "), *TextEntry.CommitHash, *TextEntry.CommitHash) + Paths;
        LeftLabel = FText::Format(LOCTEXT("TextParent", "Parent of {0}"), FText::FromString(TextEntry.CommitHash.Left(7)));
        RightLabel = FText::FromString(TextEntry.CommitHash.Left(7));
        break;
    default:
        if (TextEntry.StatusCode == TEXT('?'))
        {
            // Untracked files have nothing in git to diff against.
            Args = Git + Diff + TEXT("--no-index -- /dev/null ") + Paths;
        }
        else if (TextEntry.bConflicted)
        {
            // --ours keeps the output a plain two-sided diff instead of a combined one.
            Args = Git + Diff + TEXT("--ours -- ") + Paths;
            LeftLabel = LOCTEXT("TextOurs", "Mine (ours)");
        }
        else
        {
            Args = Git + Diff + TEXT("-- ") + Paths;
        }
        break;
    }

    FString Output;
    int32 ReturnCode = -1;
    FGraphXPortGitProcess::RunText(Args, RepoRoot, Output, ReturnCode);
    if (Output.IsEmpty() && TextEntry.Source == EGraphXPortChangeSource::Commit)
    {
        // A root commit has no parent to diff against.
        FGraphXPortGitProcess::RunText(Git + TEXT("show --no-color --no-ext-diff -U999999 --format= ") + TextEntry.CommitHash + TEXT(" -- ") + Paths, RepoRoot, Output, ReturnCode);
    }

    TArray<FString> Lines;
    Output.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

    TextDiffRows.Reset();
    TArray<FString> Removed;
    TArray<FString> Added;
    int32 LeftLine = 1;
    int32 RightLine = 1;
    // A run of removals followed by additions is a modification: pair them up row by row.
    auto Flush = [&]()
    {
        for (int32 i = 0; i < FMath::Max(Removed.Num(), Added.Num()); ++i)
        {
            TSharedPtr<FTextDiffRow> Row = MakeShared<FTextDiffRow>();
            Row->bChanged = true;
            if (Removed.IsValidIndex(i)) { Row->LeftLine = LeftLine++; Row->Left = Removed[i]; }
            if (Added.IsValidIndex(i)) { Row->RightLine = RightLine++; Row->Right = Added[i]; }
            TextDiffRows.Add(Row);
        }
        Removed.Reset();
        Added.Reset();
    };

    bool bInHunk = false;
    for (const FString& Line : Lines)
    {
        if (!bInHunk)
        {
            bInHunk = Line.StartsWith(TEXT("@@"));
            continue;
        }
        if (Line.StartsWith(TEXT("\\")))
        {
            continue; // "\ No newline at end of file"
        }
        const FString Text = Line.Mid(1).Replace(TEXT("\t"), TEXT("    "));
        if (Line.StartsWith(TEXT("-")))
        {
            Removed.Add(Text);
        }
        else if (Line.StartsWith(TEXT("+")))
        {
            Added.Add(Text);
        }
        else
        {
            Flush();
            TSharedPtr<FTextDiffRow> Row = MakeShared<FTextDiffRow>();
            Row->LeftLine = LeftLine++;
            Row->RightLine = RightLine++;
            Row->Left = Text;
            Row->Right = Text;
            TextDiffRows.Add(Row);
        }
    }
    Flush();

    if (TextDiffRows.IsEmpty())
    {
        return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(LOCTEXT("NoTextDiff", "No textual differences (binary file, or only mode changed)."))
            ];
    }

    const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);
    auto MakeHalf = [Mono](int32 LineNumber, const FString& Text, const FLinearColor& Background)
    {
        return SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
            .BorderBackgroundColor(Background)
            .Padding(FMargin(0.f, 0.f))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SBox).WidthOverride(48.f)
                    [
                        SNew(STextBlock)
                        .Text(LineNumber > 0 ? FText::AsNumber(LineNumber, &FNumberFormattingOptions::DefaultNoGrouping()) : FText::GetEmpty())
                        .Font(Mono)
                        .ColorAndOpacity(FLinearColor(0.45f, 0.45f, 0.45f))
                        .Justification(ETextJustify::Right)
                    ]
                ]
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(8.f, 0.f, 0.f, 0.f)
                [
                    SNew(STextBlock).Text(FText::FromString(Text)).Font(Mono)
                ]
            ];
    };

    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f) [ SNew(STextBlock).Text(LeftLabel).Font(FAppStyle::GetFontStyle("BoldFont")) ]
            + SHorizontalBox::Slot().FillWidth(1.f) [ SNew(STextBlock).Text(RightLabel).Font(FAppStyle::GetFontStyle("BoldFont")) ]
        ]
        + SVerticalBox::Slot().FillHeight(1.f)
        [
            // One list for both sides, so they always scroll together.
            SNew(SListView<TSharedPtr<FTextDiffRow>>)
            .ListItemsSource(&TextDiffRows)
            .SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([MakeHalf](TSharedPtr<FTextDiffRow> Row, const TSharedRef<STableViewBase>& Owner)
            {
                const FLinearColor None(0.f, 0.f, 0.f, 0.f);
                // Linear values: 0.12 renders as a mid grey, so the padding needs to be near black.
                const FLinearColor Hatch(0.006f, 0.006f, 0.006f, 1.f);
                const FLinearColor Removed(0.45f, 0.08f, 0.08f, 0.6f);
                const FLinearColor Added(0.12f, 0.4f, 0.12f, 0.6f);
                const FLinearColor LeftBg = !Row->bChanged ? None : Row->LeftLine > 0 ? Removed : Hatch;
                const FLinearColor RightBg = !Row->bChanged ? None : Row->RightLine > 0 ? Added : Hatch;
                return SNew(STableRow<TSharedPtr<FTextDiffRow>>, Owner)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.f) [ MakeHalf(Row->LeftLine, Row->Left, LeftBg) ]
                        + SHorizontalBox::Slot().AutoWidth() [ SNew(SBox).WidthOverride(6.f) ]
                        + SHorizontalBox::Slot().FillWidth(1.f) [ MakeHalf(Row->RightLine, Row->Right, RightBg) ]
                    ];
            })
        ];
}

void SGraphXPortDiffPanel::RefreshResultOrigin()
{
    ResultOrigin = EGraphXPortResultOrigin::Unknown;
    if (Mode != EGraphXPortDiffMode::Conflict3Way || !Entry.IsValid())
    {
        return;
    }

    FString WorkingId;
    if (!FGraphXPortGitBlob::HashWorkingFile(RepoRoot, Entry->RepoRelativePath, WorkingId))
    {
        return;
    }

    FString MineId;
    FString TheirsId;
    FGraphXPortGitBlob::GetBlobId(RepoRoot, TEXT(":2"), Entry->RepoRelativePath, MineId);
    FGraphXPortGitBlob::GetBlobId(RepoRoot, TEXT(":3"), Entry->RepoRelativePath, TheirsId);

    if (WorkingId == MineId)
    {
        ResultOrigin = EGraphXPortResultOrigin::Mine;
    }
    else if (WorkingId == TheirsId)
    {
        ResultOrigin = EGraphXPortResultOrigin::Incoming;
    }
    else
    {
        ResultOrigin = EGraphXPortResultOrigin::Merged;
    }
}

FText SGraphXPortDiffPanel::GetResultOriginText() const
{
    // The hash describes the file on disk; unsaved edits in memory are layered on top.
    const bool bDirty = ResultSide.Package && ResultSide.Package->IsDirty();

    FText Origin;
    switch (ResultOrigin)
    {
    case EGraphXPortResultOrigin::Mine:     Origin = LOCTEXT("OriginMine", "Result starts from Mine"); break;
    case EGraphXPortResultOrigin::Incoming: Origin = LOCTEXT("OriginIncoming", "Result starts from Incoming"); break;
    case EGraphXPortResultOrigin::Merged:   Origin = LOCTEXT("OriginMerged", "Result has saved merge edits"); break;
    default:                                Origin = LOCTEXT("OriginUnknown", "Result"); break;
    }

    return bDirty
        ? FText::Format(LOCTEXT("OriginDirty", "{0} (unsaved edits)"), Origin)
        : Origin;
}

FSlateColor SGraphXPortDiffPanel::GetResultOriginColor() const
{
    using namespace GraphXPortDiffPanelPrivate;

    if (ResultSide.Package && ResultSide.Package->IsDirty())
    {
        return FSlateColor(ChangedColor);
    }
    return FSlateColor(ResultOrigin == EGraphXPortResultOrigin::Merged ? ChangedColor : CategoryColor);
}

bool SGraphXPortDiffPanel::CanUseSideAsResult(bool bIncoming) const
{
    const FSideData& Source = bIncoming ? TheirsSide : MineSide;
    const EGraphXPortResultOrigin Target = bIncoming ? EGraphXPortResultOrigin::Incoming : EGraphXPortResultOrigin::Mine;
    const bool bDirty = ResultSide.Package && ResultSide.Package->IsDirty();

    // Already identical, with nothing unsaved to throw away: the button would do nothing.
    return Mode == EGraphXPortDiffMode::Conflict3Way && Source.Asset != nullptr && (ResultOrigin != Target || bDirty);
}

FReply SGraphXPortDiffPanel::OnUseSideAsResultClicked(bool bIncoming)
{
    if (!Entry.IsValid() || !CanUseSideAsResult(bIncoming))
    {
        return FReply::Handled();
    }

    const FText SideName = bIncoming ? LOCTEXT("SideNameIncoming", "Incoming") : LOCTEXT("SideNameMine", "Mine");
    const bool bLosesWork = ResultOrigin == EGraphXPortResultOrigin::Merged
        || (ResultSide.Package && ResultSide.Package->IsDirty());

    const FText Warning = bLosesWork
        ? LOCTEXT("UseSideLoses", "The merge edits already in Result - saved or unsaved - will be lost.\n\n")
        : FText::GetEmpty();

    const FText Message = FText::Format(
        LOCTEXT("UseSideConfirm", "Start the Result from {0}?\n\n"
            "The Result asset on disk is replaced with the {0} version and reloaded. "
            "Then copy the changes you want to keep from the other side into Result, and "
            "click Save & Mark Resolved.\n\n{1}Nothing is staged until you click Save & Mark Resolved."),
        SideName, Warning);

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    // Every pane and reference into the live asset must go before its package is reloaded
    // underneath it - reloading replaces those objects.
    const TSharedPtr<FGraphXPortGitFileEntry> CurrentEntry = Entry;
    ReleaseLoadedRevisions();

    FString Error;
    const bool bReplaced = FGraphXPortTempPackage::ReplaceWorkingFileWithRevision(
        RepoRoot, bIncoming ? TEXT(":3") : TEXT(":2"), CurrentEntry->RepoRelativePath, Error);

    SetFile(RepoRoot, CurrentEntry);

    if (StatusText.IsValid())
    {
        if (!bReplaced)
        {
            StatusText->SetText(FText::Format(LOCTEXT("UseSideFailed", "Could not start from {0}: {1}"), SideName, FText::FromString(Error)));
        }
        else if (!Error.IsEmpty())
        {
            StatusText->SetText(FText::Format(LOCTEXT("UseSideReloadWarn", "Result replaced with {0}, but reloading reported: {1}"), SideName, FText::FromString(Error)));
        }
        else
        {
            StatusText->SetText(bIncoming
                ? LOCTEXT("UseIncomingDone", "Result now holds Incoming. Copy your changes from the Mine pane (left) into Result.")
                : LOCTEXT("UseMineDone", "Result now holds Mine. Copy the incoming changes from the Theirs pane (right) into Result."));
        }
    }

    return FReply::Handled();
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

    ReleaseSideWidgets();
    VisibleTreeRoots.Reset();
    VisibleChildren.Reset();
    VisibleDifferences.Reset();
    if (DiffTreeView.IsValid()) DiffTreeView->ClearSelection();

    // Diff results and tree rows hold raw node pointers into those revisions.
    GraphDiffs.Reset();
    GraphNames.Reset();
    SelectedGraphName = NAME_None;
    CurrentView = EGraphXPortView::MyBlueprint;
    InspectedComponentName = NAME_None;
    InspectedMemberName = NAME_None;
    InspectedProperty = FPropertySoftPath();
    PendingJumpNodeGuid.Invalidate();
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

void SGraphXPortDiffPanel::LoadTempSide(FSideData& Side, const FString& RefSpec, const FString& RepoRelativePath)
{
    FGraphXPortLoadedAsset Loaded;
    FString Error;
    if (FGraphXPortTempPackage::LoadBlobAsAsset(RepoRoot, RefSpec, RepoRelativePath, Loaded, Error))
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

    const FString& Path = Entry->RepoRelativePath;
    const FString& BeforePath = Entry->BeforePath();

    // Staged and committed changes are two historical revisions. Both load as throwaway
    // diff-only packages, so nothing here can touch the working tree. A side that did not
    // exist (an add or a delete) simply shows "Not present at this revision".
    if (Entry->Source != EGraphXPortChangeSource::Unstaged)
    {
        Mode = EGraphXPortDiffMode::ReadOnlyCompare;

        if (Entry->Source == EGraphXPortChangeSource::Staged)
        {
            BaseSide.Label = LOCTEXT("StagedBefore", "HEAD");
            LoadTempSide(BaseSide, TEXT("HEAD"), BeforePath);
            ResultSide.Label = LOCTEXT("StagedAfter", "Staged (index, read-only)");
            LoadTempSide(ResultSide, FString(), Path);
        }
        else
        {
            const FText ShortHash = FText::FromString(Entry->CommitHash.Left(7));
            BaseSide.Label = FText::Format(LOCTEXT("CommitBefore", "Parent of {0}"), ShortHash);
            LoadTempSide(BaseSide, Entry->CommitHash + TEXT("^"), BeforePath);
            ResultSide.Label = FText::Format(LOCTEXT("CommitAfter", "{0} (read-only)"), ShortHash);
            LoadTempSide(ResultSide, Entry->CommitHash, Path);
        }
        return;
    }

    switch (Entry->StatusCode)
    {
    case TEXT('U'):
        Mode = EGraphXPortDiffMode::Conflict3Way;
        BaseSide.Label = LOCTEXT("Base", "Base (common ancestor)");
        LoadTempSide(BaseSide, TEXT(":1"), Path);
        MineSide.Label = LOCTEXT("Mine", "Mine (yours)");
        LoadTempSide(MineSide, TEXT(":2"), Path);
        TheirsSide.Label = LOCTEXT("Theirs", "Theirs (incoming)");
        LoadTempSide(TheirsSide, TEXT(":3"), Path);
        ResultSide.Label = LOCTEXT("Result", "Result (editable)");
        LoadRealSide(ResultSide, /*bEditable=*/true);
        break;

    case TEXT('M'):
    case TEXT('R'):
        Mode = EGraphXPortDiffMode::ModifiedCompare;
        BaseSide.Label = LOCTEXT("Head", "HEAD");
        LoadTempSide(BaseSide, TEXT("HEAD"), BeforePath);
        ResultSide.Label = LOCTEXT("Working", "Working tree (editable)");
        LoadRealSide(ResultSide, /*bEditable=*/true);
        break;

    case TEXT('D'):
        Mode = EGraphXPortDiffMode::SingleReadOnly;
        BaseSide.Label = LOCTEXT("HeadDeleted", "HEAD (deleted in working tree)");
        LoadTempSide(BaseSide, TEXT("HEAD"), BeforePath);
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

FText SGraphXPortDiffPanel::LocalChangeTag() const
{
    if (Mode == EGraphXPortDiffMode::Conflict3Way)
    {
        return LOCTEXT("SideMine", "MINE");
    }
    if (Entry.IsValid() && Entry->Source == EGraphXPortChangeSource::Staged)
    {
        return LOCTEXT("SideStaged", "STAGED");
    }
    if (Entry.IsValid() && Entry->Source == EGraphXPortChangeSource::Commit)
    {
        return LOCTEXT("SideCommit", "COMMIT");
    }
    return LOCTEXT("SideWorking", "WORKING");
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
            RemoveRevisionClassPinDefaults(*Set.MineDiffs, BaseSide, MineSide);
        }
        if (BaseGraph && TheirsGraph)
        {
            FGraphDiffControl::DiffGraphs(BaseGraph, TheirsGraph, *Set.TheirsDiffs);
            RemoveRevisionClassPinDefaults(*Set.TheirsDiffs, BaseSide, TheirsSide);
        }
        if (BaseGraph && ResultGraph)
        {
            FGraphDiffControl::DiffGraphs(BaseGraph, ResultGraph, *Set.ResultDiffs);
            RemoveRevisionClassPinDefaults(*Set.ResultDiffs, BaseSide, ResultSide);
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

void SGraphXPortDiffPanel::RemoveRevisionClassPinDefaults(TArray<FDiffSingleResult>& Diffs, const FSideData& OldSide, const FSideData& NewSide)
{
    if (!OldSide.Package || !NewSide.Package || (!OldSide.bIsTemp && !NewSide.bIsTemp)) return;

    Diffs.RemoveAll([&](const FDiffSingleResult& Diff)
    {
        const UObject* OldDefault = Diff.Pin1 ? Diff.Pin1->DefaultObject.Get() : nullptr;
        const UObject* NewDefault = Diff.Pin2 ? Diff.Pin2->DefaultObject.Get() : nullptr;
        // Historical revisions give the same Blueprint class different temporary package paths.
        return Diff.Diff == EDiffType::PIN_DEFAULT_VALUE && OldDefault && NewDefault
            && OldDefault->IsA<UClass>() && NewDefault->IsA<UClass>()
            && OldDefault->GetOutermost() == OldSide.Package
            && NewDefault->GetOutermost() == NewSide.Package
            && OldDefault->GetName() == NewDefault->GetName();
    });
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
                Row->MemberName = SideVar.VarName;
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
                Row->MemberName = BaseVar.VarName;
                Row->bFromTheirs = bFromTheirs;
                OutRows.Add(Row);
            }
        }
    };

    const FText MineTag = LocalChangeTag();

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
            Row->PropertyPath = Difference.Identifier;
            Row->bFromTheirs = bFromTheirs;
            OutRows.Add(Row);
        }
    };

    const FText MineTag = LocalChangeTag();

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
                    Row->PropertyPath = Difference.Identifier;
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

    const FText MineTag = LocalChangeTag();

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

        // Order is serialized (data table rows, enum values, struct members), so a pure
        // reorder is a real change even when every entry's value is identical.
        auto CommonKeyOrder = [](const TArray<FGraphXPortAssetEntry>& From, const TArray<FGraphXPortAssetEntry>& Other)
        {
            TArray<FName> Keys;
            for (const FGraphXPortAssetEntry& Candidate : From)
            {
                if (Other.ContainsByPredicate([&Candidate](const FGraphXPortAssetEntry& E) { return E.Key == Candidate.Key; }))
                {
                    Keys.Add(Candidate.Key);
                }
            }
            return Keys;
        };

        if (CommonKeyOrder(Base, Side) != CommonKeyOrder(Side, Base))
        {
            TSharedPtr<FGraphXPortDiffTreeNode> Row = MakeShared<FGraphXPortDiffTreeNode>();
            Row->Label = FText::Format(LOCTEXT("EntryReordered", "{0}: Order changed"), SideTag);
            Row->Color = ChangedColor;
            Row->bIsMemberRow = true;
            Row->bFromTheirs = bFromTheirs;
            OutRows.Add(Row);
        }
    };

    const FText MineTag = LocalChangeTag();

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

    auto ComparePresence = [&OutRows, &GraphName, bInBase, this](const FSideData& Side, const FText& SideTag, bool bFromTheirs)
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
        Row->bFromTheirs = bFromTheirs;
        Row->Label = bInSide
            ? FText::Format(LOCTEXT("GraphAdded", "{0}: Added"), SideTag)
            : FText::Format(LOCTEXT("GraphRemoved", "{0}: Removed"), SideTag);
        Row->Color = bInSide ? AddedColor : RemovedColor;
        OutRows.Add(Row);
    };

    const FText MineTag = LocalChangeTag();

    ComparePresence(LocalChangeSide(), MineTag, false);
    if (Mode == EGraphXPortDiffMode::Conflict3Way)
        ComparePresence(TheirsSide, LOCTEXT("SideTheirs", "THEIRS"), true);
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
        + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 2.f)
        [
            SNew(SButton).Text(LOCTEXT("RefreshDiff", "Refresh Comparison"))
            .ToolTipText(LOCTEXT("RefreshDiffTip", "Recompute differences from the loaded revisions and current unsaved Result."))
            .IsEnabled_Lambda([this] { return Entry.IsValid(); })
            .OnClicked(this, &SGraphXPortDiffPanel::RefreshComparison)
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 2.f)
        [
            SNew(SButton).Text(LOCTEXT("PaneOrientation", "Vertical / Horizontal"))
            .OnClicked(this, &SGraphXPortDiffPanel::ToggleOrientation)
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 2.f)
        [
            SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bSyncViews ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
            {
                bSyncViews = State == ECheckBoxState::Checked;
                UpdateViewLock();
            })
            .ToolTipText(LOCTEXT("SyncViewsTip", "Pan and zoom graph panes together. Uncheck to move each pane separately."))
            [ SNew(STextBlock).Text(LOCTEXT("SyncViews", "Sync Views")) ]
        ]
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
            .Text(LOCTEXT("Save", "Save Result"))
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
            .ToolTipText(LOCTEXT("SaveResolveTip", "Save the merged result you edited in the Result pane, then stage it so git records the conflict as resolved."))
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
        // Which side Result is built on. Pick the side with more changes, then copy the
        // smaller side's changes into it.
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(18.f, 2.f, 6.f, 2.f)
        [
            SNew(STextBlock)
            .Text(this, &SGraphXPortDiffPanel::GetResultOriginText)
            .ColorAndOpacity(this, &SGraphXPortDiffPanel::GetResultOriginColor)
            .Visibility(this, &SGraphXPortDiffPanel::GetConflictActionVisibility)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("UseMine", "Start from Mine"))
            .ToolTipText(LOCTEXT("UseMineTip", "Replace Result with your version (Mine), then copy the incoming changes you want from the Theirs pane. Use this when you changed more than Incoming did.\n\nThis is what Result holds when the conflict starts."))
            .Visibility(this, &SGraphXPortDiffPanel::GetConflictActionVisibility)
            .IsEnabled_Lambda([this]() { return CanUseSideAsResult(/*bIncoming=*/false); })
            .OnClicked(this, &SGraphXPortDiffPanel::OnUseSideAsResultClicked, false)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(2.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("UseIncoming", "Start from Incoming"))
            .ToolTipText(LOCTEXT("UseIncomingTip", "Replace Result with the incoming version (Theirs), then copy your own changes into it from the Mine pane. Use this when Incoming changed more than you did."))
            .Visibility(this, &SGraphXPortDiffPanel::GetConflictActionVisibility)
            .IsEnabled_Lambda([this]() { return CanUseSideAsResult(/*bIncoming=*/true); })
            .OnClicked(this, &SGraphXPortDiffPanel::OnUseSideAsResultClicked, true)
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
    // Staged, committed and deleted files have nothing editable to save.
    return Mode != EGraphXPortDiffMode::Conflict3Way && ResultSide.bEditable ? EVisibility::Visible : EVisibility::Collapsed;
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
    switch (CurrentView)
    {
    case EGraphXPortView::MyBlueprint: return LOCTEXT("ViewMyBlueprint", "My Blueprint");
    case EGraphXPortView::Defaults: return LOCTEXT("ViewDefaults", "Defaults");
    case EGraphXPortView::ClassSettings: return LOCTEXT("ViewClassSettings", "Class Settings");
    case EGraphXPortView::Components: return LOCTEXT("ViewComponents", "Components");
    case EGraphXPortView::Entries: return LOCTEXT("ViewEntries", "Asset Entries");
    default: return FText::FromString(FName::NameToDisplayString(SelectedGraphName.ToString(), false));
    }
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

    // Column 0 is a 1-based position, like the Data Table editor's leftmost column, so row
    // order can be compared across panes at a glance. Asset columns start at 1.
    auto AddHeader = [&Grid, &MakeCell](int32 GridColumn, const FString& Text)
    {
        Grid->AddSlot(GridColumn, 0)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("DetailsView.CategoryTop"))
            [
                MakeCell(Text, CategoryColor, /*bHeader=*/true)
            ]
        ];
    };

    AddHeader(0, TEXT("#"));
    for (int32 ColumnIndex = 0; ColumnIndex < Side.Columns.Num(); ++ColumnIndex)
    {
        AddHeader(ColumnIndex + 1, Side.Columns[ColumnIndex]);
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

        Grid->AddSlot(0, Row)
        [
            MakeCell(FString::FromInt(Row), IndexColor, /*bHeader=*/false)
        ];

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

            if (ColumnIndex == 0 && AssetEntry.IconClass)
            {
                Grid->AddSlot(ColumnIndex + 1, Row)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
                    [
                        SNew(SImage)
                        .Image(FSlateIconFinder::FindIconForClass(AssetEntry.IconClass).GetIcon())
                        .DesiredSizeOverride(FVector2D(16.f, 16.f))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        MakeCell(AssetEntry.Cells[ColumnIndex], CellColor, /*bHeader=*/false)
                    ]
                ];
                continue;
            }

            Grid->AddSlot(ColumnIndex + 1, Row)
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

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildSideWidget(FSideData& Side, TSharedPtr<SGraphEditor>& OutPane, TSharedPtr<FUICommandList>& OutCommands)
{
    TSharedRef<SVerticalBox> Container = SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f, 2.f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(FMargin(8.f, 6.f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(Entry.IsValid() ? FText::FromString(FPaths::GetBaseFilename(Entry->RepoRelativePath)) : FText::GetEmpty())
                    .Font(FAppStyle::GetFontStyle("BoldFont"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
                [ SNew(STextBlock).Text(Side.Label) ]
            ]
        ];

    // Entry-based assets have no graphs; they are always shown as their entry list. A
    // Widget Blueprint shows its designer tree the same way.
    const bool bShowEntries = FGraphXPortAssetDiff::IsEntryBased(Side.Kind)
        || (CurrentView == EGraphXPortView::Entries && !Side.Entries.IsEmpty());
    if (bShowEntries && Side.Asset)
    {
        if (!Side.Thumbnail.IsValid())
        {
            if (UTexture* Texture = Cast<UTexture>(Side.Asset))
            {
                TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
                Brush->SetResourceObject(Texture);
                Brush->ImageSize = FVector2D(FMath::Max(1, (int32)Texture->Source.GetSizeX()), FMath::Max(1, (int32)Texture->Source.GetSizeY()));
                Side.Thumbnail = Brush;
            }
            else if (Side.Kind == EGraphXPortAssetKind::Material || Side.Kind == EGraphXPortAssetKind::MaterialInstance
                || Side.Kind == EGraphXPortAssetKind::Properties)
            {
                // The same still the Content Browser shows: a lit sphere for materials, the mesh for meshes.
                TArray<uint8> Pixels;
                int32 Width = 0;
                int32 Height = 0;
                if (FGraphXPortAssetDiff::RenderThumbnail(Side.Asset, Side.bIsTemp, Pixels, Width, Height))
                {
                    Side.Thumbnail = FSlateDynamicImageBrush::CreateWithImageData(
                        *FString::Printf(TEXT("GraphXPortThumb_%s"), *FGuid::NewGuid().ToString()),
                        FVector2D(Width, Height), Pixels);
                }
            }
        }

        if (const TSharedPtr<FSlateBrush> Brush = Side.Thumbnail)
        {
            Container->AddSlot()
            .AutoHeight()
            .Padding(4.f)
            [
                SNew(SBox)
                .HeightOverride(256.f)
                [
                    SNew(SScaleBox)
                    .Stretch(EStretch::ScaleToFit)
                    [
                        // The lambda keeps the brush alive for as long as the image is.
                        SNew(SImage).Image_Lambda([Brush]() { return Brush.Get(); })
                    ]
                ]
            ];
        }

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

    if (CurrentView != EGraphXPortView::Graph)
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
    // Revision labels belong in the header; keep the graph canvas clear.

    EnsureInspector(Side);
    Side.MyBlueprint->SetFocusedGraph(FoundGraph);
    Side.MyBlueprint->Refresh();
    Side.Inspector->ShowDetailsForSingleObject(FoundGraph);
    SGraphEditor::FGraphEditorEvents Events;
    const TWeakPtr<SKismetInspector> WeakInspector = Side.Inspector;
    const TWeakObjectPtr<UEdGraph> WeakGraph = FoundGraph;
    Events.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateLambda(
        [WeakInspector, WeakGraph](const FGraphPanelSelectionSet& Selection)
        {
            if (const TSharedPtr<SKismetInspector> Inspector = WeakInspector.Pin())
            {
                TArray<UObject*> Objects = Selection.Array();
                if (Objects.IsEmpty() && WeakGraph.IsValid()) Objects.Add(WeakGraph.Get());
                Inspector->ShowDetailsForObjects(Objects);
            }
        });
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
        SNew(SSplitter)
        .Orientation(Orient_Vertical)
        + SSplitter::Slot().Value(0.76f)
        [
        SAssignNew(OutPane, SGraphEditor)
        .AdditionalCommands(Commands)
        .IsEditable(Side.bEditable)
        .DisplayAsReadOnly(!Side.bEditable)
        .Appearance(Appearance)
        .GraphToEdit(FoundGraph)
        .DiffResults(Side.DiffResults)
        .GraphEvents(Events)
        ]
        + SSplitter::Slot().Value(0.24f)
        [
            Side.Inspector.ToSharedRef()
        ]
    ];

    return Container;
}

void SGraphXPortDiffPanel::RebuildPanes()
{
    if (!PanesContainer.IsValid())
    {
        return;
    }

    // Detach cached native widgets before placing them in the new pane hierarchy.
    PanesContainer->SetContent(SNullWidget::NullWidget);
    LeftPane.Reset();
    ResultPane.Reset();
    TheirsPane.Reset();
    LeftPaneCommands.Reset();
    ResultPaneCommands.Reset();
    TheirsPaneCommands.Reset();

    struct FActiveSide
    {
        FSideData* Side;
        TSharedPtr<SGraphEditor>* Pane;
        TSharedPtr<FUICommandList>* Commands;
    };

    // The left pane shows whichever reference the toggle selects.
    FSideData* LeftSide = bLeftShowsMine ? &MineSide : &BaseSide;

    TArray<FActiveSide> ActiveSides;
    switch (Mode)
    {
    case EGraphXPortDiffMode::Conflict3Way:
        ActiveSides.Add({ LeftSide, &LeftPane, &LeftPaneCommands });
        ActiveSides.Add({ &ResultSide, &ResultPane, &ResultPaneCommands });
        ActiveSides.Add({ &TheirsSide, &TheirsPane, &TheirsPaneCommands });
        break;
    case EGraphXPortDiffMode::ModifiedCompare:
    case EGraphXPortDiffMode::ReadOnlyCompare:
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

    TSharedRef<SSplitter> Splitter = SNew(SSplitter).Orientation(bVerticalPanes ? Orient_Vertical : Orient_Horizontal);
    for (const FActiveSide& Active : ActiveSides)
    {
        Splitter->AddSlot()
        [
            BuildSideWidget(*Active.Side, *Active.Pane, *Active.Commands)
        ];
    }

    PanesContainer->SetContent(Splitter);
    UpdateViewLock();
}

void SGraphXPortDiffPanel::UpdateViewLock()
{
    const TSharedPtr<SGraphEditor> Panes[] = { LeftPane, ResultPane, TheirsPane };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Panes); ++Index)
    {
        if (!Panes[Index].IsValid()) continue;
        for (int32 OtherIndex = Index + 1; OtherIndex < UE_ARRAY_COUNT(Panes); ++OtherIndex)
        {
            if (!Panes[OtherIndex].IsValid()) continue;
            if (bSyncViews)
            {
                Panes[Index]->LockToGraphEditor(Panes[OtherIndex]);
                Panes[OtherIndex]->LockToGraphEditor(Panes[Index]);
            }
            else
            {
                Panes[Index]->UnlockFromGraphEditor(Panes[OtherIndex]);
                Panes[OtherIndex]->UnlockFromGraphEditor(Panes[Index]);
            }
        }
    }
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
