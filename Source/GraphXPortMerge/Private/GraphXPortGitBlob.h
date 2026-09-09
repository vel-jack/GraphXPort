#pragma once

#include "CoreMinimal.h"

/** Extracts historical file content from git without going through FString text
 *  encoding, so binary .uasset payloads survive intact. */
class FGraphXPortGitBlob
{
public:
    /** True if <RefSpec>:<RepoRelativePath> exists (e.g. RefSpec = "HEAD", ":1", ":2", ":3"). */
    static bool BlobExists(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath);

    /** Extracts <RefSpec>:<RepoRelativePath> to DestFilePath via shell redirection, which
     *  copies bytes disk-to-disk instead of round-tripping them through an FString. */
    static bool ExtractBlobToFile(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, const FString& DestFilePath);
};
