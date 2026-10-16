#include "GraphXPortTempPackage.h"

#include "DiffUtils.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "GraphXPortGitBlob.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace GraphXPortTempPackagePrivate
{
    static FString TempRootDir()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("GraphXPortMerge"));
    }

    /**
     * Picks the asset to compare out of a loaded package. For a level (.umap) the package's
     * asset is a UWorld, but the thing worth diffing is its level script Blueprint, so the
     * world is recorded as the asset and its script Blueprint as the graph source.
     */
    static void ClassifyPackageAsset(UPackage* Package, FGraphXPortLoadedAsset& Out, FString& OutError)
    {
        UWorld* World = nullptr;
        UObject* Candidate = nullptr;
        EGraphXPortAssetKind CandidateKind = EGraphXPortAssetKind::None;

        ForEachObjectWithPackage(Package, [&World, &Candidate, &CandidateKind](UObject* Object)
        {
            if (UWorld* AsWorld = Cast<UWorld>(Object))
            {
                World = AsWorld;
                return false;
            }

            const EGraphXPortAssetKind Kind = FGraphXPortAssetDiff::DetectKind(Object);
            if (FGraphXPortAssetDiff::KindPriority(Kind) > FGraphXPortAssetDiff::KindPriority(CandidateKind))
            {
                Candidate = Object;
                CandidateKind = Kind;
            }

            return true;
        }, /*bIncludeNestedObjects=*/false);

        if (World)
        {
            Out.Asset = World;
            Out.Kind = EGraphXPortAssetKind::LevelScript;

            // Never create one here: on a historical revision that would fabricate content
            // that was not in the commit.
            if (ULevel* Level = World->PersistentLevel)
            {
                Out.Blueprint = Level->GetLevelScriptBlueprint(/*bDontCreate=*/true);
            }

            if (!Out.Blueprint)
            {
                OutError = TEXT("This level has no Level Blueprint.");
            }
            return;
        }

        if (!Candidate)
        {
            OutError = TEXT("No comparable asset in this file.");
            return;
        }

        Out.Asset = Candidate;
        Out.Kind = CandidateKind;
        if (CandidateKind == EGraphXPortAssetKind::Blueprint)
        {
            Out.Blueprint = Cast<UBlueprint>(Candidate);
        }
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

void FGraphXPortTempPackage::PurgeLoadedRevisions()
{
    // Collect first: this releases the revision packages and closes their linkers, so the
    // loose files can actually be deleted afterwards.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);

    CleanStaleTempFiles();
}

bool FGraphXPortTempPackage::LoadBlobAsAsset(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FGraphXPortLoadedAsset& OutResult, FString& OutError)
{
    using namespace GraphXPortTempPackagePrivate;

    OutResult = FGraphXPortLoadedAsset();
    OutError.Empty();

    if (!FGraphXPortGitBlob::BlobExists(RepoRoot, RefSpec, RepoRelativePath))
    {
        OutError = TEXT("Not present at this revision.");
        return false;
    }

    const FString AssetBaseName = FPaths::GetBaseFilename(RepoRelativePath);
    const FString Extension = FPaths::GetExtension(RepoRelativePath, /*bIncludeDot=*/true);
    const FString UniqueFolder = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TempFilePath = TempRootDir() / UniqueFolder / (AssetBaseName + Extension);

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

    OutResult.Package = LoadedPackage;
    ClassifyPackageAsset(LoadedPackage, OutResult, OutError);

    // A revision loads with compile-on-load disabled, so it has no skeleton class - and
    // SMyBlueprint lists variables, functions and dispatchers off the skeleton, so without
    // this a revision's My Blueprint panel comes up empty while the working copy fills in.
    // The engine's own Blueprint diff does the same for the same reason (SBlueprintDiff).
    if (OutResult.Blueprint && !OutResult.Blueprint->SkeletonGeneratedClass)
    {
        FKismetEditorUtilities::GenerateBlueprintSkeleton(OutResult.Blueprint);
    }

    return OutResult.IsValid();
}

bool FGraphXPortTempPackage::ResolveRealAsset(const FString& RepoRoot, const FString& RepoRelativePath, FGraphXPortLoadedAsset& OutResult, FString& OutError)
{
    using namespace GraphXPortTempPackagePrivate;

    OutResult = FGraphXPortLoadedAsset();
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

    OutResult.Package = Package;
    ClassifyPackageAsset(Package, OutResult, OutError);

    return OutResult.IsValid();
}

bool FGraphXPortTempPackage::ReplaceWorkingFileWithRevision(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FString& OutError)
{
    OutError.Empty();

    const FString AbsPath = FPaths::ConvertRelativePathToFull(RepoRoot / RepoRelativePath);

    FString LongPackageName;
    if (!FPackageName::TryConvertFilenameToLongPackageName(AbsPath, LongPackageName))
    {
        OutError = TEXT("Not a recognized Unreal asset path.");
        return false;
    }

    if (!FGraphXPortGitBlob::BlobExists(RepoRoot, RefSpec, RepoRelativePath))
    {
        OutError = TEXT("That side does not contain this file.");
        return false;
    }

    UPackage* Package = FindPackage(nullptr, *LongPackageName);
    if (Package)
    {
        // Reloading the level the editor is showing would pull the world out from under
        // every viewport and outliner.
        if (UWorld* World = UWorld::FindWorldInPackage(Package))
        {
            if (GEditor && GEditor->GetEditorWorldContext().World() == World)
            {
                OutError = TEXT("This level is open in the editor. Open a different level first.");
                return false;
            }
        }

        if (!Package->IsFullyLoaded())
        {
            FlushAsyncLoading();
            Package->FullyLoad();
        }
        ResetLoaders(Package);
    }

    // Extract beside the target, then move over it, so a failed extraction can never leave
    // a truncated asset behind. The extension keeps the directory watcher from treating the
    // staging file as a package.
    const FString StagingPath = AbsPath + TEXT(".gxptmp");
    if (!FGraphXPortGitBlob::ExtractBlobToFile(RepoRoot, RefSpec, RepoRelativePath, StagingPath))
    {
        IFileManager::Get().Delete(*StagingPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        OutError = TEXT("Failed to read that side from git.");
        return false;
    }

    if (!IFileManager::Get().Move(*AbsPath, *StagingPath, /*Replace=*/true, /*EvenIfReadOnly=*/true))
    {
        IFileManager::Get().Delete(*StagingPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        OutError = TEXT("Could not replace the file - it may be open in another program.");
        return false;
    }

    if (Package)
    {
        FText ReloadError;
        UPackageTools::ReloadPackages({ Package }, ReloadError, EReloadPackagesInteractionMode::AssumePositive);
        if (!ReloadError.IsEmpty())
        {
            // The file is already replaced; only the in-memory copy may be stale.
            OutError = ReloadError.ToString();
        }
    }

    return true;
}
