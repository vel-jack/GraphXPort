#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GraphXPortGitHistory.h"

namespace
{
    /** Builds one commit record exactly as ListCommits' --format prints it, followed by
     *  the blank line and --name-status block git appends. */
    FString CommitRecord(const FString& Hash, const FString& Subject, const FString& Body, const FString& NameStatus)
    {
        return FString::Printf(TEXT("\x1e%s\x1f%s\x1f" "Velmurugan\x1f" "10-Sep-2026\x1f%s\x1f%s\x1f\n\n%s"),
            *Hash, *Hash.Left(7), *Subject, *Body, *NameStatus);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortGitHistoryParseTest, "GraphXPort.Merge.GitHistory.ParseLog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortGitHistoryParseTest::RunTest(const FString& Parameters)
{
    const FString Log =
        CommitRecord(TEXT("531d701c0000000000000000000000000000abcd"), TEXT("feat(gis): Cesium navigation"),
            TEXT("- Add input components\n- Fix camera altitude"),
            TEXT("M\tContent/BP_Pawn.uasset\nR087\tContent/Old Name.uasset\tContent/New Name.uasset\nD\tContent/Gone.umap\n"))
        + CommitRecord(TEXT("d6ffc86600000000000000000000000000001234"), TEXT("initial dummy commit"), FString(),
            TEXT("A\tContent/A_BadActor.uasset\n"));

    TArray<FGraphXPortGitCommit> Commits;
    FGraphXPortGitHistory::ParseLogOutput(Log, Commits);

    if (!TestEqual(TEXT("Both commits are parsed"), Commits.Num(), 2))
    {
        return false;
    }

    const FGraphXPortGitCommit& First = Commits[0];
    TestEqual(TEXT("Short hash"), First.ShortHash, FString(TEXT("531d701")));
    TestEqual(TEXT("Author"), First.Author, FString(TEXT("Velmurugan")));
    TestEqual(TEXT("Date"), First.Date, FString(TEXT("10-Sep-2026")));
    TestEqual(TEXT("Subject"), First.Subject, FString(TEXT("feat(gis): Cesium navigation")));
    TestTrue(TEXT("Multi-line body survives"), First.Body.Contains(TEXT("Fix camera altitude")));

    if (TestEqual(TEXT("First commit lists three assets"), First.Files.Num(), 3))
    {
        TestEqual(TEXT("Modified status"), First.Files[0].StatusCode, TEXT('M'));
        TestTrue(TEXT("Files carry their commit"), First.Files[0].CommitHash == First.Hash);
        TestTrue(TEXT("Files are commit-sourced"), First.Files[0].Source == EGraphXPortChangeSource::Commit);

        // Rename lines carry a similarity score and old/new paths; spaces must survive.
        TestEqual(TEXT("Rename status"), First.Files[1].StatusCode, TEXT('R'));
        TestEqual(TEXT("Rename new path"), First.Files[1].RepoRelativePath, FString(TEXT("Content/New Name.uasset")));
        TestEqual(TEXT("Rename old path"), First.Files[1].OriginalPath, FString(TEXT("Content/Old Name.uasset")));

        TestEqual(TEXT("Deleted map"), First.Files[2].StatusCode, TEXT('D'));
    }

    const FGraphXPortGitCommit& Second = Commits[1];
    TestTrue(TEXT("Empty body stays empty"), Second.Body.IsEmpty());
    TestEqual(TEXT("Second commit lists one asset"), Second.Files.Num(), 1);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
