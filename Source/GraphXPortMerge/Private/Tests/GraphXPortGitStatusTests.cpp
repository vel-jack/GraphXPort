#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GraphXPortGitStatus.h"

namespace
{
    /** Builds one `git status --porcelain=v2` ordinary-change line ("1" record). */
    FString OrdinaryLine(const FString& XY, const FString& Path)
    {
        return FString::Printf(TEXT("1 %s N... 100644 100644 100644 abc1234 def5678 %s"), *XY, *Path);
    }

    /** Rename/copy record ("2"), whose path field is "<new>\t<original>". */
    FString RenameLine(const FString& XY, const FString& NewPath, const FString& OldPath)
    {
        return FString::Printf(TEXT("2 %s N... 100644 100644 100644 abc1234 def5678 R100 %s\t%s"),
            *XY, *NewPath, *OldPath);
    }

    /** Unmerged record ("u") - a conflict, which carries three stage hashes. */
    FString UnmergedLine(const FString& XY, const FString& Path)
    {
        return FString::Printf(TEXT("u %s N... 100644 100644 100644 100644 h1a2b3c h4d5e6f h7a8b9c %s"),
            *XY, *Path);
    }

    const FGraphXPortGitFileEntry* FindEntry(const TArray<FGraphXPortGitFileEntry>& Entries, const FString& Path)
    {
        return Entries.FindByPredicate(
            [&Path](const FGraphXPortGitFileEntry& Entry) { return Entry.RepoRelativePath == Path; });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortGitStatusWorktreeTest, "GraphXPort.Merge.GitStatus.WorktreeChanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortGitStatusWorktreeTest::RunTest(const FString& Parameters)
{
    TArray<FString> Lines;
    Lines.Add(OrdinaryLine(TEXT(".M"), TEXT("Content/WorktreeModified.uasset")));
    Lines.Add(OrdinaryLine(TEXT("M."), TEXT("Content/StagedOnly.uasset")));
    Lines.Add(OrdinaryLine(TEXT("MM"), TEXT("Content/StagedAndModified.uasset")));
    Lines.Add(OrdinaryLine(TEXT(".D"), TEXT("Content/WorktreeDeleted.uasset")));

    TArray<FGraphXPortGitFileEntry> Entries;
    FGraphXPortGitStatus::ParseStatusOutput(FString::Join(Lines, TEXT("\n")), Entries);

    // A file whose only change is already staged has nothing left to review here.
    TestNull(TEXT("Staged-only entry is skipped"), FindEntry(Entries, TEXT("Content/StagedOnly.uasset")));

    const FGraphXPortGitFileEntry* Modified = FindEntry(Entries, TEXT("Content/WorktreeModified.uasset"));
    if (TestNotNull(TEXT("Worktree-modified entry is listed"), Modified))
    {
        TestEqual(TEXT("Worktree-modified status code"), Modified->StatusCode, TEXT('M'));
        TestFalse(TEXT("Worktree-modified is not conflicted"), Modified->bConflicted);
    }

    // XY is index-then-worktree, so the second character is the one we report.
    const FGraphXPortGitFileEntry* Both = FindEntry(Entries, TEXT("Content/StagedAndModified.uasset"));
    if (TestNotNull(TEXT("Staged-and-modified entry is listed"), Both))
    {
        TestEqual(TEXT("Reported status comes from the worktree column"), Both->StatusCode, TEXT('M'));
    }

    const FGraphXPortGitFileEntry* Deleted = FindEntry(Entries, TEXT("Content/WorktreeDeleted.uasset"));
    if (TestNotNull(TEXT("Worktree-deleted entry is listed"), Deleted))
    {
        TestEqual(TEXT("Worktree-deleted status code"), Deleted->StatusCode, TEXT('D'));
    }

    TestEqual(TEXT("Only the three worktree changes are listed"), Entries.Num(), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortGitStatusConflictTest, "GraphXPort.Merge.GitStatus.ConflictsAndUntracked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortGitStatusConflictTest::RunTest(const FString& Parameters)
{
    TArray<FString> Lines;
    Lines.Add(UnmergedLine(TEXT("UU"), TEXT("Content/BothModified.uasset")));
    Lines.Add(UnmergedLine(TEXT("AA"), TEXT("Content/BothAdded.uasset")));
    Lines.Add(TEXT("? Content/Untracked.uasset"));
    Lines.Add(TEXT("! Content/Ignored.uasset"));

    TArray<FGraphXPortGitFileEntry> Entries;
    FGraphXPortGitStatus::ParseStatusOutput(FString::Join(Lines, TEXT("\n")), Entries);

    const FGraphXPortGitFileEntry* Conflict = FindEntry(Entries, TEXT("Content/BothModified.uasset"));
    if (TestNotNull(TEXT("Unmerged entry is listed"), Conflict))
    {
        TestEqual(TEXT("Unmerged status code"), Conflict->StatusCode, TEXT('U'));
        TestTrue(TEXT("Unmerged entry is flagged conflicted"), Conflict->bConflicted);
    }

    // Both-added conflicts have no common ancestor but are still unresolved.
    const FGraphXPortGitFileEntry* BothAdded = FindEntry(Entries, TEXT("Content/BothAdded.uasset"));
    if (TestNotNull(TEXT("Both-added conflict is listed"), BothAdded))
    {
        TestTrue(TEXT("Both-added entry is flagged conflicted"), BothAdded->bConflicted);
    }

    const FGraphXPortGitFileEntry* Untracked = FindEntry(Entries, TEXT("Content/Untracked.uasset"));
    if (TestNotNull(TEXT("Untracked entry is listed"), Untracked))
    {
        TestEqual(TEXT("Untracked status code"), Untracked->StatusCode, TEXT('?'));
    }

    TestNull(TEXT("Ignored entry is skipped"), FindEntry(Entries, TEXT("Content/Ignored.uasset")));
    TestEqual(TEXT("Ignored lines do not produce entries"), Entries.Num(), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortGitStatusRenameTest, "GraphXPort.Merge.GitStatus.RenamesAndPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortGitStatusRenameTest::RunTest(const FString& Parameters)
{
    TArray<FString> Lines;
    Lines.Add(RenameLine(TEXT("RM"), TEXT("Content/NewName.uasset"), TEXT("Content/OldName.uasset")));
    Lines.Add(RenameLine(TEXT("R."), TEXT("Content/StagedRename.uasset"), TEXT("Content/WasCalled.uasset")));
    Lines.Add(OrdinaryLine(TEXT(".M"), TEXT("Content/Folder With Spaces/My Asset.uasset")));

    TArray<FGraphXPortGitFileEntry> Entries;
    FGraphXPortGitStatus::ParseStatusOutput(FString::Join(Lines, TEXT("\n")), Entries);

    // The rename record's path field is "<new>\t<original>"; we track the current path.
    const FGraphXPortGitFileEntry* Renamed = FindEntry(Entries, TEXT("Content/NewName.uasset"));
    if (TestNotNull(TEXT("Rename with worktree edit is listed under its new path"), Renamed))
    {
        TestEqual(TEXT("Rename status code"), Renamed->StatusCode, TEXT('R'));
    }

    TestNull(TEXT("Rename with no worktree change is skipped"),
        FindEntry(Entries, TEXT("Content/StagedRename.uasset")));

    TestNotNull(TEXT("Paths containing spaces survive parsing"),
        FindEntry(Entries, TEXT("Content/Folder With Spaces/My Asset.uasset")));

    TestEqual(TEXT("Only worktree-affecting entries are listed"), Entries.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortGitStatusStagedTest, "GraphXPort.Merge.GitStatus.StagedChanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortGitStatusStagedTest::RunTest(const FString& Parameters)
{
    TArray<FString> Lines;
    Lines.Add(OrdinaryLine(TEXT("M."), TEXT("Content/StagedOnly.uasset")));
    Lines.Add(OrdinaryLine(TEXT("MM"), TEXT("Content/StagedAndModified.uasset")));
    Lines.Add(OrdinaryLine(TEXT("A."), TEXT("Content/StagedAdd.uasset")));
    Lines.Add(OrdinaryLine(TEXT(".M"), TEXT("Content/WorktreeOnly.uasset")));
    Lines.Add(RenameLine(TEXT("R."), TEXT("Content/NewName.uasset"), TEXT("Content/OldName.uasset")));
    Lines.Add(UnmergedLine(TEXT("UU"), TEXT("Content/Conflict.uasset")));

    TArray<FGraphXPortGitFileEntry> Unstaged;
    TArray<FGraphXPortGitFileEntry> Staged;
    FGraphXPortGitStatus::ParseStatusOutput(FString::Join(Lines, TEXT("\n")), Unstaged, Staged);

    // X is the index column, so it is the one reported for a staged entry.
    const FGraphXPortGitFileEntry* StagedOnly = FindEntry(Staged, TEXT("Content/StagedOnly.uasset"));
    if (TestNotNull(TEXT("Staged-only entry is in the staged list"), StagedOnly))
    {
        TestEqual(TEXT("Staged status comes from the index column"), StagedOnly->StatusCode, TEXT('M'));
        TestTrue(TEXT("Staged entry is marked as staged"), StagedOnly->Source == EGraphXPortChangeSource::Staged);
    }

    // Staged and then edited again: two different diffs, so it belongs in both lists.
    TestNotNull(TEXT("Staged-and-modified is in the staged list"), FindEntry(Staged, TEXT("Content/StagedAndModified.uasset")));
    TestNotNull(TEXT("Staged-and-modified is in the unstaged list"), FindEntry(Unstaged, TEXT("Content/StagedAndModified.uasset")));

    const FGraphXPortGitFileEntry* Added = FindEntry(Staged, TEXT("Content/StagedAdd.uasset"));
    if (TestNotNull(TEXT("Staged add is listed"), Added))
    {
        TestEqual(TEXT("Staged add status code"), Added->StatusCode, TEXT('A'));
    }

    // HEAD still has the file under its old name, which is where the "before" side lives.
    const FGraphXPortGitFileEntry* Renamed = FindEntry(Staged, TEXT("Content/NewName.uasset"));
    if (TestNotNull(TEXT("Staged rename is listed under its new path"), Renamed))
    {
        TestEqual(TEXT("Staged rename keeps the original path"), Renamed->OriginalPath, FString(TEXT("Content/OldName.uasset")));
        TestEqual(TEXT("Before path is the original path"), Renamed->BeforePath(), FString(TEXT("Content/OldName.uasset")));
    }

    TestNull(TEXT("Worktree-only change is not staged"), FindEntry(Staged, TEXT("Content/WorktreeOnly.uasset")));
    TestNull(TEXT("An unresolved conflict is not staged"), FindEntry(Staged, TEXT("Content/Conflict.uasset")));
    TestEqual(TEXT("Staged list size"), Staged.Num(), 4);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
