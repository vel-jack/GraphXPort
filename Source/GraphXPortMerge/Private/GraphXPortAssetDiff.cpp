#include "GraphXPortAssetDiff.h"

#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

namespace GraphXPortAssetDiffPrivate
{
    static void CollectEnumEntries(const UUserDefinedEnum* Enum, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        // A user-defined enum always carries a trailing hidden _MAX entry; skip it.
        const int32 Count = Enum->NumEnums() - 1;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();

            FGraphXPortAssetEntry Entry;
            Entry.Key = FName(*Enum->GetNameStringByIndex(Index));
            Entry.Display = DisplayName;
            Entry.Cells.Add(FString::FromInt(Index));
            Entry.Cells.Add(DisplayName);
            // Index is part of the value so that reordering registers as a change.
            Entry.Value = FString::Printf(TEXT("%d|%s"), Index, *DisplayName);
            OutEntries.Add(MoveTemp(Entry));
        }
    }

    static void CollectStructEntries(const UUserDefinedStruct* Struct, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        for (const FStructVariableDescription& Variable : FStructureEditorUtils::GetVarDesc(Struct))
        {
            FString TypeText = Variable.Category.ToString();
            if (!Variable.SubCategory.IsNone())
            {
                TypeText += TEXT(" ") + Variable.SubCategory.ToString();
            }
            else if (!Variable.SubCategoryObject.IsNull())
            {
                TypeText += TEXT(" ") + Variable.SubCategoryObject.GetAssetName();
            }

            FGraphXPortAssetEntry Entry;
            // The guid survives a rename, so a renamed field reads as changed, not as
            // one field removed and another added.
            Entry.Key = FName(*Variable.VarGuid.ToString());
            Entry.Display = Variable.FriendlyName;
            Entry.Cells.Add(Variable.FriendlyName);
            Entry.Cells.Add(TypeText);
            Entry.Cells.Add(Variable.DefaultValue);
            Entry.Value = FString::Printf(TEXT("%s|%s|%s"), *Variable.FriendlyName, *TypeText, *Variable.DefaultValue);
            OutEntries.Add(MoveTemp(Entry));
        }
    }

    static void CollectDataTableEntries(const UDataTable* Table, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        const UScriptStruct* RowStruct = Table->GetRowStruct();
        const TMap<FName, uint8*>& RowMap = Table->GetRowMap();

        TArray<FName> RowNames;
        RowMap.GenerateKeyArray(RowNames);
        RowNames.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });

        for (const FName& RowName : RowNames)
        {
            uint8* const* RowData = RowMap.Find(RowName);

            FGraphXPortAssetEntry Entry;
            Entry.Key = RowName;
            Entry.Display = RowName.ToString();
            Entry.Cells.Add(RowName.ToString());

            FString RowValue;
            if (RowStruct && RowData && *RowData)
            {
                for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
                {
                    FString PropertyValue;
                    It->ExportText_InContainer(0, PropertyValue, *RowData, *RowData, nullptr, PPF_None);
                    Entry.Cells.Add(PropertyValue);
                    RowValue += FString::Printf(TEXT("%s=%s; "), *It->GetName(), *PropertyValue);
                }
            }

            Entry.Value = RowValue;
            OutEntries.Add(MoveTemp(Entry));
        }
    }
}

EGraphXPortAssetKind FGraphXPortAssetDiff::DetectKind(const UObject* Asset)
{
    if (!Asset)
    {
        return EGraphXPortAssetKind::None;
    }

    if (Asset->IsA<ULevelScriptBlueprint>())
    {
        return EGraphXPortAssetKind::LevelScript;
    }
    if (Asset->IsA<UBlueprint>())
    {
        return EGraphXPortAssetKind::Blueprint;
    }
    if (Asset->IsA<UUserDefinedEnum>())
    {
        return EGraphXPortAssetKind::Enum;
    }
    if (Asset->IsA<UUserDefinedStruct>())
    {
        return EGraphXPortAssetKind::Struct;
    }
    if (Asset->IsA<UDataTable>())
    {
        return EGraphXPortAssetKind::DataTable;
    }

    return EGraphXPortAssetKind::Unsupported;
}

bool FGraphXPortAssetDiff::IsEntryBased(EGraphXPortAssetKind Kind)
{
    return Kind == EGraphXPortAssetKind::Enum
        || Kind == EGraphXPortAssetKind::Struct
        || Kind == EGraphXPortAssetKind::DataTable;
}

FText FGraphXPortAssetDiff::DescribeEntries(EGraphXPortAssetKind Kind)
{
    switch (Kind)
    {
    case EGraphXPortAssetKind::Enum:
        return LOCTEXT("EntriesEnum", "Enumerators");
    case EGraphXPortAssetKind::Struct:
        return LOCTEXT("EntriesStruct", "Fields");
    case EGraphXPortAssetKind::DataTable:
        return LOCTEXT("EntriesDataTable", "Rows");
    default:
        return LOCTEXT("EntriesGeneric", "Entries");
    }
}

bool FGraphXPortAssetDiff::GetColumns(const UObject* Asset, TArray<FString>& OutColumns)
{
    OutColumns.Reset();

    if (Asset && Asset->IsA<UUserDefinedEnum>())
    {
        OutColumns.Add(TEXT("Index"));
        OutColumns.Add(TEXT("Display Name"));
        return true;
    }

    if (Asset && Asset->IsA<UUserDefinedStruct>())
    {
        OutColumns.Add(TEXT("Name"));
        OutColumns.Add(TEXT("Type"));
        OutColumns.Add(TEXT("Default"));
        return true;
    }

    if (const UDataTable* Table = Cast<UDataTable>(Asset))
    {
        OutColumns.Add(TEXT("Row"));

        if (const UScriptStruct* RowStruct = Table->GetRowStruct())
        {
            for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
            {
                // Authored names drop the guid suffix user-defined struct properties carry.
                const FString ColumnName = It->GetAuthoredName();
                OutColumns.Add(ColumnName.IsEmpty() ? It->GetName() : ColumnName);
            }
        }

        return true;
    }

    return false;
}

bool FGraphXPortAssetDiff::GetEntries(const UObject* Asset, TArray<FGraphXPortAssetEntry>& OutEntries)
{
    using namespace GraphXPortAssetDiffPrivate;

    OutEntries.Reset();

    if (const UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Asset))
    {
        CollectEnumEntries(Enum, OutEntries);
        return true;
    }
    if (const UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Asset))
    {
        CollectStructEntries(Struct, OutEntries);
        return true;
    }
    if (const UDataTable* Table = Cast<UDataTable>(Asset))
    {
        CollectDataTableEntries(Table, OutEntries);
        return true;
    }

    return false;
}

#undef LOCTEXT_NAMESPACE
