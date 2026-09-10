#pragma once

#include "CoreMinimal.h"

class UObject;

/** Which kind of asset a revision holds, and therefore how it can be compared. */
enum class EGraphXPortAssetKind : uint8
{
    None,
    /** A regular Blueprint - compared as graphs. */
    Blueprint,
    /** A level's script Blueprint, living inside a .umap - also compared as graphs. */
    LevelScript,
    /** Compared as a flat list of entries rather than graphs. */
    Enum,
    Struct,
    DataTable,
    /** Recognised asset we have no comparison for. */
    Unsupported
};

/**
 * One comparable entry of a non-graph asset: an enumerator, a struct field, or a data
 * table row. Key is the identity used to match entries across revisions; Value is what
 * is compared to decide an entry changed.
 */
struct FGraphXPortAssetEntry
{
    FName Key;
    FString Display;
    FString Value;
    /** One cell per column returned by GetColumns, for the side-by-side grid. */
    TArray<FString> Cells;
};

class FGraphXPortAssetDiff
{
public:
    static EGraphXPortAssetKind DetectKind(const UObject* Asset);

    /** True for kinds compared as a list of entries rather than as graphs. */
    static bool IsEntryBased(EGraphXPortAssetKind Kind);

    /** Section title for this kind's entries, e.g. "Enumerators". */
    static FText DescribeEntries(EGraphXPortAssetKind Kind);

    /** Collects the comparable entries of an entry-based asset. */
    static bool GetEntries(const UObject* Asset, TArray<FGraphXPortAssetEntry>& OutEntries);

    /** Column headings matching each entry's Cells - for a data table, its row struct's properties. */
    static bool GetColumns(const UObject* Asset, TArray<FString>& OutColumns);
};
