#pragma once

#include "CoreMinimal.h"
#include "GraphXPortAssetDiff.h"

class UBlueprint;
class UPackage;

/** A revision of an asset, loaded and classified. */
struct FGraphXPortLoadedAsset
{
    /** The asset itself: the Blueprint, enum, struct, data table, or - for a level - the world. */
    UObject* Asset = nullptr;
    /** Set for Blueprint and LevelScript kinds; the graphs to compare live here. */
    UBlueprint* Blueprint = nullptr;
    UPackage* Package = nullptr;
    EGraphXPortAssetKind Kind = EGraphXPortAssetKind::None;

    bool IsValid() const { return Asset != nullptr; }
};

/**
 * Loads alternate revisions of an asset (extracted from git) as diff-only packages,
 * using the engine's own DiffUtils::LoadPackageForDiff - the same path the editor's
 * "Diff Against Depot" uses. That matters for more than convenience: it loads from a
 * loose file with LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks,
 * so an old revision never compiles-on-load, and it needs no mount point, so revisions
 * never appear in the Content Browser.
 *
 * Loaded revisions are deliberately NOT force-unloaded; callers keep them alive by
 * referencing them from an FGCObject while displayed and let GC reclaim them afterwards.
 * Explicitly unloading a package whose graph a live SGraphEditor still points at crashes.
 */
class FGraphXPortTempPackage
{
public:
    /** Removes temp revision files left behind by previous sessions. */
    static void CleanStaleTempFiles();

    /**
     * Collects the loaded revisions and deletes their files. Callers MUST have dropped
     * every reference first (panes torn down, sides cleared) - this collects garbage
     * synchronously, so anything still pointing at a revision would be left dangling.
     *
     * Needed because diff revisions are real assets while loaded: their classes and
     * structs show up in class/struct pickers, so a session that reviews several files
     * would otherwise offer the user duplicate entries.
     */
    static void PurgeLoadedRevisions();

    /** Extracts <RefSpec>:<RepoRelativePath> from git and loads it as a diff-only revision. */
    static bool LoadBlobAsAsset(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FGraphXPortLoadedAsset& OutResult, FString& OutError);

    /** Resolves RepoRelativePath to the real, live /Game asset - used for the editable
     *  Result side, which is never a diff-only copy. */
    static bool ResolveRealAsset(const FString& RepoRoot, const FString& RepoRelativePath, FGraphXPortLoadedAsset& OutResult, FString& OutError);

    /**
     * Overwrites the working-tree file with <RefSpec>:<RepoRelativePath> and hot-reloads
     * the live package in place. This is the order the engine's own source-control revert
     * uses (USourceControlHelpers::ApplyOperationAndReloadPackages): detach the package's
     * linker so the file is no longer held open, write the file, then ReloadPackages.
     * Writing while the linker is attached fails because the editor holds the file open.
     *
     * Callers MUST have dropped every widget and reference into the live asset first:
     * reloading replaces its objects. Unsaved in-memory edits are discarded. Refuses a
     * level that is currently open in the editor.
     */
    static bool ReplaceWorkingFileWithRevision(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FString& OutError);
};
