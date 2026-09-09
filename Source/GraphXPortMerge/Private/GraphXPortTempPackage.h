#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UPackage;

/** Result of loading a historical git revision as an isolated, diff-only Blueprint. */
struct FGraphXPortLoadedBlueprint
{
    UBlueprint* Blueprint = nullptr;
    UPackage* Package = nullptr;

    bool IsValid() const { return Blueprint != nullptr; }
};

/**
 * Loads alternate revisions of a Blueprint (extracted from git) as diff-only packages,
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

    /** Extracts <RefSpec>:<RepoRelativePath> from git and loads it as a diff-only Blueprint. */
    static bool LoadBlobAsBlueprint(const FString& RepoRoot, const FString& RefSpec, const FString& RepoRelativePath, FGraphXPortLoadedBlueprint& OutResult, FString& OutError);

    /** Resolves RepoRelativePath to the real, live /Game Blueprint - used for the editable
     *  "Mine" side, which is never a diff-only copy. */
    static bool ResolveRealBlueprint(const FString& RepoRoot, const FString& RepoRelativePath, UBlueprint*& OutBlueprint, FString& OutError);
};
