#pragma once

#include "CoreMinimal.h"
#include "GraphXPortGitStatus.h"

/** One commit that touched at least one Unreal asset, with the assets it changed. */
struct FGraphXPortGitCommit
{
    FString Hash;
    FString ShortHash;
    FString Author;
    FString Date;
    FString Subject;
    FString Body;
    /** Changed .uasset/.umap files, each with Source == Commit. Merge commits are compared
     *  against their first parent, the same as `git show --first-parent`. */
    TArray<FGraphXPortGitFileEntry> Files;
};

/** Read-only access to commit history. Never runs anything that changes repository state. */
class FGraphXPortGitHistory
{
public:
    /** Lists commits touching .uasset/.umap files, newest first. Skip/Count page through
     *  the history so a large repo is never read in full. */
    static bool ListCommits(const FString& RepoRoot, int32 Skip, int32 Count, TArray<FGraphXPortGitCommit>& OutCommits);

    /** Parses the output of the log command ListCommits runs. Separated so it can be
     *  tested against known git output. */
    static void ParseLogOutput(const FString& LogOutput, TArray<FGraphXPortGitCommit>& OutCommits);
};
