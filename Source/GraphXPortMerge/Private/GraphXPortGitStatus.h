#pragma once

#include "CoreMinimal.h"

/** Which pair of revisions a file entry compares. */
enum class EGraphXPortChangeSource : uint8
{
    /** HEAD (or the conflict stages) against the working tree - the only editable source. */
    Unstaged,
    /** HEAD against the index. Read-only. */
    Staged,
    /** A commit's first parent against the commit. Read-only. */
    Commit
};

/** One changed file reported by git, relative to the repo root. */
struct FGraphXPortGitFileEntry
{
    FString RepoRelativePath;
    /** Path in the "before" revision when the file was renamed or copied; empty otherwise. */
    FString OriginalPath;
    /** Single-letter status for display: M, A, D, R, C, U (conflict), or '?' (untracked). */
    TCHAR StatusCode = TEXT('?');
    bool bConflicted = false;
    EGraphXPortChangeSource Source = EGraphXPortChangeSource::Unstaged;
    /** Full hash, for Source == Commit. */
    FString CommitHash;

    /** Where the file lived in the "before" revision. */
    const FString& BeforePath() const { return OriginalPath.IsEmpty() ? RepoRelativePath : OriginalPath; }
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

    /** Lists worktree (unstaged) and index (staged) changes from a single `git status`. A
     *  file staged and then edited again appears in both lists. */
    static bool ListChanges(const FString& RepoRoot, TArray<FGraphXPortGitFileEntry>& OutUnstaged, TArray<FGraphXPortGitFileEntry>& OutStaged);

    /** Parses `git status --porcelain=v2` output. Separated from process execution so it
     *  can be tested directly against known git output. */
    static void ParseStatusOutput(const FString& StatusOutput, TArray<FGraphXPortGitFileEntry>& OutUnstaged, TArray<FGraphXPortGitFileEntry>& OutStaged);

    /** Unstaged entries only. */
    static void ParseStatusOutput(const FString& StatusOutput, TArray<FGraphXPortGitFileEntry>& OutUnstaged);

    /** Stages a path. For an unmerged file this is what marks the conflict resolved. */
    static bool StageFile(const FString& RepoRoot, const FString& RepoRelativePath);
};
