#include "GraphXPortGitHistory.h"

#include "GraphXPortGitProcess.h"

namespace GraphXPortGitHistoryPrivate
{
    // Each commit starts with a record separator and its header fields are split by unit
    // separators; neither can appear in a commit message, unlike newlines or tabs.
    static const TCHAR* RecordSeparator = TEXT("\x1e");
    static const TCHAR* FieldSeparator = TEXT("\x1f");

    enum ELogField : int32
    {
        Field_Hash,
        Field_ShortHash,
        Field_Author,
        Field_Date,
        Field_Subject,
        Field_Body,
        Field_NameStatus,
        Field_Count
    };
}

bool FGraphXPortGitHistory::ListCommits(const FString& RepoRoot, int32 Skip, int32 Count, TArray<FGraphXPortGitCommit>& OutCommits)
{
    OutCommits.Reset();

    // --diff-merges=first-parent lists what a merge brought in relative to the branch it
    // was merged into; without it git prints no files for merge commits at all. The
    // pathspecs are quoted so git expands them, not the shell.
    const FString CommonArgs = FString::Printf(
        TEXT("-c core.quotePath=false log --name-status -M --date=format:%%d-%%b-%%Y ")
        TEXT("\"--format=%%x1e%%H%%x1f%%h%%x1f%%an%%x1f%%ad%%x1f%%s%%x1f%%b%%x1f\" ")
        TEXT("--skip=%d -n %d"),
        FMath::Max(0, Skip), FMath::Max(1, Count));
    const FString PathSpecs = TEXT("-- \"*.uasset\" \"*.umap\"");

    FString LogOut;
    int32 ReturnCode = -1;
    bool bOk = FGraphXPortGitProcess::RunText(
        CommonArgs + TEXT(" --diff-merges=first-parent ") + PathSpecs, RepoRoot, LogOut, ReturnCode) && ReturnCode == 0;

    if (!bOk)
    {
        // git older than 2.31 lacks --diff-merges; merge commits then just list no files.
        bOk = FGraphXPortGitProcess::RunText(CommonArgs + TEXT(" ") + PathSpecs, RepoRoot, LogOut, ReturnCode) && ReturnCode == 0;
    }

    if (!bOk)
    {
        return false;
    }

    ParseLogOutput(LogOut, OutCommits);
    return true;
}

void FGraphXPortGitHistory::ParseLogOutput(const FString& LogOutput, TArray<FGraphXPortGitCommit>& OutCommits)
{
    using namespace GraphXPortGitHistoryPrivate;

    OutCommits.Reset();

    TArray<FString> Records;
    LogOutput.ParseIntoArray(Records, RecordSeparator, /*bCullEmpty=*/true);

    for (const FString& Record : Records)
    {
        TArray<FString> Fields;
        Record.ParseIntoArray(Fields, FieldSeparator, /*bCullEmpty=*/false);
        if (Fields.Num() < Field_Count || Fields[Field_Hash].TrimStartAndEnd().IsEmpty())
        {
            continue;
        }

        FGraphXPortGitCommit Commit;
        Commit.Hash = Fields[Field_Hash].TrimStartAndEnd();
        Commit.ShortHash = Fields[Field_ShortHash].TrimStartAndEnd();
        Commit.Author = Fields[Field_Author];
        Commit.Date = Fields[Field_Date];
        Commit.Subject = Fields[Field_Subject];
        Commit.Body = Fields[Field_Body].TrimStartAndEnd();

        TArray<FString> Lines;
        Fields[Field_NameStatus].ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
        for (const FString& Line : Lines)
        {
            TArray<FString> Parts;
            Line.ParseIntoArray(Parts, TEXT("\t"), /*bCullEmpty=*/false);
            if (Parts.Num() < 2 || Parts[0].IsEmpty())
            {
                continue;
            }

            FGraphXPortGitFileEntry File;
            File.Source = EGraphXPortChangeSource::Commit;
            File.CommitHash = Commit.Hash;

            // Renames and copies carry a similarity score ("R087") and two paths.
            const TCHAR Letter = Parts[0][0];
            if ((Letter == TEXT('R') || Letter == TEXT('C')) && Parts.Num() >= 3)
            {
                File.OriginalPath = Parts[1];
                File.RepoRelativePath = Parts[2];
            }
            else
            {
                File.RepoRelativePath = Parts[1];
            }
            File.StatusCode = Letter == TEXT('T') ? TEXT('M') : Letter;

            Commit.Files.Add(MoveTemp(File));
        }

        OutCommits.Add(MoveTemp(Commit));
    }
}
