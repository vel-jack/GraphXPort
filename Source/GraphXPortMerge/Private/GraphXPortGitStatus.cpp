#include "GraphXPortGitStatus.h"

#include "GraphXPortGitProcess.h"
#include "Misc/Paths.h"

namespace GraphXPortGitStatusPrivate
{
    static bool RunGit(const FString& Params, const FString& WorkingDirectory, FString& OutStdOut)
    {
        int32 ReturnCode = -1;
        return FGraphXPortGitProcess::RunText(Params, WorkingDirectory, OutStdOut, ReturnCode) && ReturnCode == 0;
    }

    /** Splits a porcelain v2 line into space-separated fields, rejoining every field from
     *  FirstPathToken onward as the trailing path (paths may themselves contain spaces). */
    static FString RemainderFromToken(const TArray<FString>& Tokens, int32 FirstPathToken)
    {
        FString Result;
        for (int32 Index = FirstPathToken; Index < Tokens.Num(); ++Index)
        {
            if (Index > FirstPathToken)
            {
                Result.AppendChar(TEXT(' '));
            }
            Result.Append(Tokens[Index]);
        }
        return Result;
    }
}

EGraphXPortGitAvailability FGraphXPortGitStatus::DetectAvailability(FString& OutRepoRoot)
{
    using namespace GraphXPortGitStatusPrivate;

    OutRepoRoot.Empty();

    FString VersionOut;
    if (!RunGit(TEXT("--version"), FString(), VersionOut))
    {
        return EGraphXPortGitAvailability::GitNotInstalled;
    }

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

    FString ToplevelOut;
    if (!RunGit(TEXT("rev-parse --show-toplevel"), ProjectDir, ToplevelOut))
    {
        return EGraphXPortGitAvailability::NotARepository;
    }

    ToplevelOut.TrimStartAndEndInline();
    if (ToplevelOut.IsEmpty())
    {
        return EGraphXPortGitAvailability::NotARepository;
    }

    FPaths::NormalizeDirectoryName(ToplevelOut);
    OutRepoRoot = ToplevelOut;
    return EGraphXPortGitAvailability::Available;
}

bool FGraphXPortGitStatus::StageFile(const FString& RepoRoot, const FString& RepoRelativePath)
{
    using namespace GraphXPortGitStatusPrivate;

    FString NormalizedPath = RepoRelativePath;
    NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));

    FString Output;
    return RunGit(FString::Printf(TEXT("add -- \"%s\""), *NormalizedPath), RepoRoot, Output);
}

bool FGraphXPortGitStatus::ListChanges(const FString& RepoRoot, TArray<FGraphXPortGitFileEntry>& OutUnstaged, TArray<FGraphXPortGitFileEntry>& OutStaged)
{
    using namespace GraphXPortGitStatusPrivate;

    OutUnstaged.Reset();
    OutStaged.Reset();

    FString StatusOut;
    if (!RunGit(TEXT("-c core.quotePath=false status --porcelain=v2 --untracked-files=all"), RepoRoot, StatusOut))
    {
        return false;
    }

    ParseStatusOutput(StatusOut, OutUnstaged, OutStaged);
    return true;
}

void FGraphXPortGitStatus::ParseStatusOutput(const FString& StatusOut, TArray<FGraphXPortGitFileEntry>& OutUnstaged)
{
    TArray<FGraphXPortGitFileEntry> IgnoredStaged;
    ParseStatusOutput(StatusOut, OutUnstaged, IgnoredStaged);
}

void FGraphXPortGitStatus::ParseStatusOutput(const FString& StatusOut, TArray<FGraphXPortGitFileEntry>& OutEntries, TArray<FGraphXPortGitFileEntry>& OutStaged)
{
    using namespace GraphXPortGitStatusPrivate;

    OutEntries.Reset();
    OutStaged.Reset();

    // XY is index-then-worktree: X describes the staged change, Y the unstaged one.
    auto AddStaged = [&OutStaged](const FString& XY, const FString& Path, const FString& OrigPath)
    {
        const TCHAR IndexStatus = XY.Len() == 2 ? XY[0] : TEXT('.');
        if (IndexStatus == TEXT('.'))
        {
            return;
        }

        FGraphXPortGitFileEntry Entry;
        Entry.RepoRelativePath = Path;
        Entry.OriginalPath = OrigPath;
        Entry.StatusCode = IndexStatus == TEXT('T') ? TEXT('M') : IndexStatus;
        Entry.Source = EGraphXPortChangeSource::Staged;
        OutStaged.Add(MoveTemp(Entry));
    };

    TArray<FString> Lines;
    StatusOut.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);

    for (const FString& Line : Lines)
    {
        if (Line.IsEmpty())
        {
            continue;
        }

        TArray<FString> Tokens;
        Line.ParseIntoArray(Tokens, TEXT(" "), /*bCullEmpty=*/true);
        if (Tokens.Num() == 0)
        {
            continue;
        }

        const FString& Kind = Tokens[0];

        if (Kind == TEXT("?"))
        {
            FGraphXPortGitFileEntry Entry;
            Entry.RepoRelativePath = RemainderFromToken(Tokens, 1);
            Entry.StatusCode = TEXT('?');
            OutEntries.Add(MoveTemp(Entry));
        }
        else if (Kind == TEXT("!"))
        {
            continue;
        }
        else if (Kind == TEXT("1") && Tokens.Num() >= 9)
        {
            const FString& XY = Tokens[1];
            const FString Path = RemainderFromToken(Tokens, 8);
            AddStaged(XY, Path, FString());

            const TCHAR WorktreeStatus = XY.Len() == 2 ? XY[1] : TEXT('.');
            if (WorktreeStatus == TEXT('.'))
            {
                continue; // staged-only change; nothing left in the worktree
            }

            FGraphXPortGitFileEntry Entry;
            Entry.RepoRelativePath = Path;
            Entry.StatusCode = WorktreeStatus == TEXT('T') ? TEXT('M') : WorktreeStatus;
            OutEntries.Add(MoveTemp(Entry));
        }
        else if (Kind == TEXT("2") && Tokens.Num() >= 10)
        {
            const FString& XY = Tokens[1];

            FString PathAndOrig = RemainderFromToken(Tokens, 9);
            FString CurrentPath = PathAndOrig;
            FString OrigPath;
            PathAndOrig.Split(TEXT("\t"), &CurrentPath, &OrigPath);

            AddStaged(XY, CurrentPath, OrigPath);

            const TCHAR WorktreeStatus = XY.Len() == 2 ? XY[1] : TEXT('.');
            if (WorktreeStatus == TEXT('.'))
            {
                continue;
            }

            // The unstaged view compares against HEAD, where the file still has its old name.
            FGraphXPortGitFileEntry Entry;
            Entry.RepoRelativePath = CurrentPath;
            Entry.OriginalPath = OrigPath;
            Entry.StatusCode = TEXT('R');
            OutEntries.Add(MoveTemp(Entry));
        }
        else if (Kind == TEXT("u") && Tokens.Num() >= 11)
        {
            FGraphXPortGitFileEntry Entry;
            Entry.RepoRelativePath = RemainderFromToken(Tokens, 10);
            Entry.StatusCode = TEXT('U');
            Entry.bConflicted = true;
            OutEntries.Add(MoveTemp(Entry));
        }
    }
}
