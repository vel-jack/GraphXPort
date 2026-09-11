#pragma once

#include "CoreMinimal.h"

/** Extracts historical file content from git without going through FString text
 *  encoding, so binary .uasset payloads survive intact. */
class FGraphXPortGitBlob
{
public:
    /** True if <RefSpec>:<RepoRelativePath> exists (e.g. RefSpec = "HEAD", ":1", ":2", ":3"). */
    static bool BlobExists(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath);

    /** Extracts <RefSpec>:<RepoRelativePath> to DestFilePath as raw bytes, never
     *  round-tripping them through an FString. */
    static bool ExtractBlobToFile(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, const FString& DestFilePath);

    /** Object id of <RefSpec>:<RepoRelativePath> (`git rev-parse`). */
    static bool GetBlobId(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FString& OutId);

    /** Object id the working-tree file would have (`git hash-object`, which does not
     *  write to the object store). Comparable with GetBlobId. */
    static bool HashWorkingFile(const FString& RepoRoot, const FString& RepoRelativePath, FString& OutId);
};
