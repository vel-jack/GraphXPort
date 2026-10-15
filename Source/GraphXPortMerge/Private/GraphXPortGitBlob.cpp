#include "GraphXPortGitBlob.h"

#include "GraphXPortGitProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GraphXPortGitBlobPrivate
{
    static bool IsLfsPointer(const TArray<uint8>& Blob)
    {
        static const ANSICHAR Prefix[] = "version https://git-lfs.github.com/spec/v1\n";
        return Blob.Num() >= UE_ARRAY_COUNT(Prefix) - 1
            && FMemory::Memcmp(Blob.GetData(), Prefix, UE_ARRAY_COUNT(Prefix) - 1) == 0;
    }

    /** git wants forward slashes in pathspecs on every platform. */
    static FString NormalizeForGit(const FString& RepoRelativePath)
    {
        FString Result = RepoRelativePath;
        Result.ReplaceInline(TEXT("\\"), TEXT("/"));
        return Result;
    }
}

bool FGraphXPortGitBlob::BlobExists(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath)
{
    using namespace GraphXPortGitBlobPrivate;

    const FString Args = FString::Printf(
        TEXT("cat-file -e \"%s:%s\""), *RefSpec, *NormalizeForGit(RepoRelativePath));

    TArray<uint8> Ignored;
    int32 ReturnCode = -1;
    return FGraphXPortGitProcess::Run(Args, RepoRoot, Ignored, ReturnCode) && ReturnCode == 0;
}

bool FGraphXPortGitBlob::ExtractBlobToFile(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, const FString& DestFilePath)
{
    using namespace GraphXPortGitBlobPrivate;

    const FString Args = FString::Printf(
        TEXT("show \"%s:%s\""), *RefSpec, *NormalizeForGit(RepoRelativePath));

    TArray<uint8> Blob;
    int32 ReturnCode = -1;
    if (!FGraphXPortGitProcess::Run(Args, RepoRoot, Blob, ReturnCode) || ReturnCode != 0 || Blob.Num() == 0)
    {
        return false;
    }

    if (IsLfsPointer(Blob))
    {
        TArray<uint8> Content;
        const FString SmudgeArgs = FString::Printf(TEXT("lfs smudge \"%s\""), *NormalizeForGit(RepoRelativePath));
        if (!FGraphXPortGitProcess::Run(SmudgeArgs, RepoRoot, Content, ReturnCode, &Blob)
            || ReturnCode != 0 || Content.IsEmpty() || IsLfsPointer(Content))
        {
            return false;
        }
        Blob = MoveTemp(Content);
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestFilePath), /*Tree=*/true);
    return FFileHelper::SaveArrayToFile(Blob, *DestFilePath);
}

bool FGraphXPortGitBlob::GetBlobId(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FString& OutId)
{
    using namespace GraphXPortGitBlobPrivate;

    int32 ReturnCode = -1;
    const FString Args = FString::Printf(TEXT("rev-parse --verify --quiet \"%s:%s\""), *RefSpec, *NormalizeForGit(RepoRelativePath));
    if (!FGraphXPortGitProcess::RunText(Args, RepoRoot, OutId, ReturnCode) || ReturnCode != 0)
    {
        OutId.Empty();
        return false;
    }

    OutId.TrimStartAndEndInline();
    return !OutId.IsEmpty();
}

bool FGraphXPortGitBlob::HashWorkingFile(const FString& RepoRoot, const FString& RepoRelativePath, FString& OutId)
{
    using namespace GraphXPortGitBlobPrivate;

    int32 ReturnCode = -1;
    const FString Args = FString::Printf(TEXT("hash-object -- \"%s\""), *NormalizeForGit(RepoRelativePath));
    if (!FGraphXPortGitProcess::RunText(Args, RepoRoot, OutId, ReturnCode) || ReturnCode != 0)
    {
        OutId.Empty();
        return false;
    }

    OutId.TrimStartAndEndInline();
    return !OutId.IsEmpty();
}
