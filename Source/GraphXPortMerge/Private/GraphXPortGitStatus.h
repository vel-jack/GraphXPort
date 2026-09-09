#pragma once

#include "CoreMinimal.h"

/** One unstaged (worktree) change reported by git, relative to the repo root. */
struct FGraphXPortGitFileEntry
{
    FString RepoRelativePath;
    /** Single-letter status for display: M, A, D, R, U (conflict), or '?' (untracked). */
    TCHAR StatusCode = TEXT('?');
    bool bConflicted = false;
};

enum class EGraphXPortGitAvailability : uint8
{
    /** git executable was not found on PATH. */
    GitNotInstalled,
    /** git is installed, but the current Unreal project is not inside a git working tree. */
    NotARepository,
    Available
};

class FGraphXPortGitStatus
{
public:
    /** Locates the git executable and the repo root containing the current Unreal project. */
    static EGraphXPortGitAvailability DetectAvailability(FString& OutRepoRoot);

    /** Lists worktree (unstaged) changes only: staged-but-not-worktree-modified entries are skipped. */
    static bool ListUnstagedChanges(const FString& RepoRoot, TArray<FGraphXPortGitFileEntry>& OutEntries);

    /** Parses `git status --porcelain=v2` output. Separated from process execution so it
     *  can be tested directly against known git output. */
    static void ParseStatusOutput(const FString& StatusOutput, TArray<FGraphXPortGitFileEntry>& OutEntries);

    /** Stages a path. For an unmerged file this is what marks the conflict resolved. */
    static bool StageFile(const FString& RepoRoot, const FString& RepoRelativePath);
};
