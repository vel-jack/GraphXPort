#include "GraphXPortTempPackage.h"

#include "DiffUtils.h"
#include "Engine/Blueprint.h"
#include "GraphXPortGitBlob.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace GraphXPortTempPackagePrivate
{
    static FString TempRootDir()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("GraphXPortMerge"));
    }
}

void FGraphXPortTempPackage::CleanStaleTempFiles()
{
    using namespace GraphXPortTempPackagePrivate;

    const FString Root = TempRootDir();
    if (IFileManager::Get().DirectoryExists(*Root))
    {
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    }
}

bool FGraphXPortTempPackage::LoadBlobAsBlueprint(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FGraphXPortLoadedBlueprint& OutResult, FString& OutError)
{
    using namespace GraphXPortTempPackagePrivate;

    OutResult = FGraphXPortLoadedBlueprint();
    OutError.Empty();

    if (!FGraphXPortGitBlob::BlobExists(RepoRoot, RefSpec, RepoRelativePath))
    {
        OutError = TEXT("Not present at this revision.");
        return false;
    }

    const FString AssetBaseName = FPaths::GetBaseFilename(RepoRelativePath);
    const FString UniqueFolder = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TempFilePath = TempRootDir() / UniqueFolder / (AssetBaseName + FPackageName::GetAssetPackageExtension());

    if (!FGraphXPortGitBlob::ExtractBlobToFile(RepoRoot, RefSpec, RepoRelativePath, TempFilePath))
    {
        OutError = TEXT("Failed to extract this revision from git.");
        return false;
    }

    const FString OriginalFilePath = FPaths::ConvertRelativePathToFull(RepoRoot / RepoRelativePath);

    const FPackagePath TempPackagePath = FPackagePath::FromLocalPath(TempFilePath);
    const FPackagePath OriginalPackagePath = FPackagePath::FromLocalPath(OriginalFilePath);

    UPackage* LoadedPackage = DiffUtils::LoadPackageForDiff(TempPackagePath, OriginalPackagePath);
    if (!LoadedPackage)
    {
        OutError = TEXT("Failed to load this revision as a package.");
        return false;
    }

    UBlueprint* FoundBlueprint = nullptr;
    ForEachObjectWithPackage(LoadedPackage, [&FoundBlueprint](UObject* Object)
    {
        if (UBlueprint* AsBlueprint = Cast<UBlueprint>(Object))
        {
            FoundBlueprint = AsBlueprint;
            return false;
        }
        return true;
    }, /*bIncludeNestedObjects=*/false);

    if (!FoundBlueprint)
    {
        OutError = TEXT("This revision does not contain a Blueprint.");
        return false;
    }

    OutResult.Blueprint = FoundBlueprint;
    OutResult.Package = LoadedPackage;
    return true;
}

bool FGraphXPortTempPackage::ResolveRealBlueprint(const FString& RepoRoot, const FString& RepoRelativePath, UBlueprint*& OutBlueprint, FString& OutError)
{
    OutBlueprint = nullptr;
    OutError.Empty();

    const FString AbsPath = FPaths::ConvertRelativePathToFull(RepoRoot / RepoRelativePath);

    FString LongPackageName;
    if (!FPackageName::TryConvertFilenameToLongPackageName(AbsPath, LongPackageName))
    {
        OutError = TEXT("Not a recognized Unreal asset path.");
        return false;
    }

    if (!FPackageName::DoesPackageExist(LongPackageName))
    {
        OutError = TEXT("File does not exist in the working tree.");
        return false;
    }

    UPackage* Package = LoadPackage(nullptr, *LongPackageName, LOAD_None);
    if (!Package)
    {
        OutError = TEXT("Failed to load asset.");
        return false;
    }

    const FString AssetBaseName = FPaths::GetBaseFilename(RepoRelativePath);
    OutBlueprint = FindObject<UBlueprint>(Package, *AssetBaseName);
    if (!OutBlueprint)
    {
        OutError = TEXT("Asset is not a Blueprint.");
        return false;
    }

    return true;
}
