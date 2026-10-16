#include "GraphXPortAssetDiff.h"

#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/LevelScriptBlueprint.h"
#include "AssetCompilingManager.h"
#include "Blueprint/WidgetTree.h"
#include "MaterialShared.h"
#include "Misc/ObjectThumbnail.h"
#include "ObjectTools.h"
#include "RHIGlobals.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "GameFramework/DefaultPhysicsVolume.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/UserDefinedEnum.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "WidgetBlueprint.h"
#include "StructUtils/UserDefinedStruct.h"
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

        // RowMap order is the table's authored row order - the same order the Data Table
        // editor shows and its drag-reorder writes - so it must not be sorted here.
        for (const TPair<FName, uint8*>& Row : RowMap)
        {
            const FName RowName = Row.Key;
            uint8* const* RowData = &Row.Value;

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

    /** Property text with this package's own path replaced, so a subobject reference reads
     *  the same in a diff-only revision and in the live asset. */
    static FString ExportValue(const FProperty* Property, const void* Container, const UObject* Owner)
    {
        FString Value;
        Property->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
        if (Owner)
        {
            Value.ReplaceInline(*Owner->GetOutermost()->GetName(), TEXT("<self>"));
        }
        return Value;
    }

    static FString PropertyGroup(const FProperty* Property)
    {
        const FString Category = Property->GetMetaData(TEXT("Category"));
        if (!Category.IsEmpty())
        {
            return Category;
        }
        return Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetName() : FString();
    }

    static void AddRow(TArray<FGraphXPortAssetEntry>& OutEntries, const FString& Key, const FString& Name, const FString& Group, const FString& Value)
    {
        FGraphXPortAssetEntry Entry;
        Entry.Key = FName(*Key);
        Entry.Display = Name;
        Entry.Cells = { Name, Group, Value };
        Entry.Value = Value;
        OutEntries.Add(MoveTemp(Entry));
    }

    /** Every editable property of Object, one entry each, except those named in Skip. */
    static void CollectProperties(const UObject* Object, TArray<FGraphXPortAssetEntry>& OutEntries, const TSet<FName>& Skip = {})
    {
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
                || Skip.Contains(Property->GetFName()))
            {
                continue;
            }
            AddRow(OutEntries, TEXT("Prop:") + Property->GetName(), Property->GetName(), PropertyGroup(Property),
                ExportValue(Property, Object, Object));
        }
    }

    /** Editable properties of Object that differ from its archetype (class defaults, or a
     *  component's template), as "Name=Value; ". */
    static FString ChangedProperties(const UObject* Object, const UObject* Owner)
    {
        FString Out;
        const UObject* Defaults = Object->GetArchetype();
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
                && !Property->Identical_InContainer(Object, Defaults))
            {
                Out += FString::Printf(TEXT("%s=%s; "), *Property->GetName(), *ExportValue(Property, Object, Owner));
            }
        }
        return Out;
    }

    static void CollectTextureEntries(const UTexture* Texture, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        AddRow(OutEntries, TEXT("Source:Size"), TEXT("Source size"), TEXT("Source"),
            FString::Printf(TEXT("%d x %d"), (int32)Texture->Source.GetSizeX(), (int32)Texture->Source.GetSizeY()));
        AddRow(OutEntries, TEXT("Source:Format"), TEXT("Source format"), TEXT("Source"),
            UEnum::GetValueAsString(Texture->Source.GetFormat()));
        AddRow(OutEntries, TEXT("Source:Mips"), TEXT("Source mips"), TEXT("Source"),
            FString::FromInt(Texture->Source.GetNumMips()));
        CollectProperties(Texture, OutEntries);
    }

    static void CollectMaterialInstanceEntries(const UMaterialInstance* Instance, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        AddRow(OutEntries, TEXT("Parent"), TEXT("Parent"), TEXT("Material"),
            Instance->Parent ? Instance->Parent->GetPathName() : FString(TEXT("None")));

        for (const FScalarParameterValue& Param : Instance->ScalarParameterValues)
        {
            AddRow(OutEntries, TEXT("Scalar:") + Param.ParameterInfo.Name.ToString(), Param.ParameterInfo.Name.ToString(),
                TEXT("Scalar parameter"), FString::SanitizeFloat(Param.ParameterValue));
        }
        for (const FVectorParameterValue& Param : Instance->VectorParameterValues)
        {
            AddRow(OutEntries, TEXT("Vector:") + Param.ParameterInfo.Name.ToString(), Param.ParameterInfo.Name.ToString(),
                TEXT("Vector parameter"), Param.ParameterValue.ToString());
        }
        for (const FTextureParameterValue& Param : Instance->TextureParameterValues)
        {
            AddRow(OutEntries, TEXT("Texture:") + Param.ParameterInfo.Name.ToString(), Param.ParameterInfo.Name.ToString(),
                TEXT("Texture parameter"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : FString(TEXT("None")));
        }

        static const TSet<FName> Shown = { TEXT("Parent"), TEXT("ScalarParameterValues"), TEXT("VectorParameterValues"), TEXT("TextureParameterValues") };
        CollectProperties(Instance, OutEntries, Shown);
    }

    static FString DescribeExpression(const UMaterialExpression* Expression)
    {
        if (!Expression)
        {
            return TEXT("None");
        }
        FString Text = Expression->GetClass()->GetName();
        Text.RemoveFromStart(TEXT("MaterialExpression"));
        const FName Parameter = Expression->GetParameterName();
        if (!Parameter.IsNone())
        {
            Text += TEXT(" '") + Parameter.ToString() + TEXT("'");
        }
        // The guid prefix tells apart two nodes of the same type.
        return Text + TEXT(" #") + Expression->MaterialExpressionGuid.ToString().Left(6);
    }

    static FString DescribeInput(const FExpressionInput* Input)
    {
        return Input && Input->Expression
            ? FString::Printf(TEXT("%s [out %d]"), *DescribeExpression(Input->Expression), Input->OutputIndex)
            : FString();
    }

    static void CollectMaterialEntries(UMaterial* Material, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        // What feeds the material's own outputs (Base Color, Normal, ...).
        for (int32 Index = 0; Index < MP_MAX; ++Index)
        {
            const EMaterialProperty Property = static_cast<EMaterialProperty>(Index);
            const FString Linked = DescribeInput(Material->GetExpressionInputForProperty(Property));
            if (!Linked.IsEmpty())
            {
                FString Name = UEnum::GetValueAsString(Property);
                Name.RemoveFromStart(TEXT("EMaterialProperty::"));
                Name.RemoveFromStart(TEXT("MP_"));
                AddRow(OutEntries, TEXT("Output:") + Name, Name, TEXT("Material output"), Linked);
            }
        }

        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            if (!Expression)
            {
                continue;
            }
            FString Value;
            for (FExpressionInputIterator It{ Expression }; It; ++It)
            {
                const FString Linked = DescribeInput(It.Input);
                if (!Linked.IsEmpty())
                {
                    Value += FString::Printf(TEXT("%s <- %s; "), *Expression->GetInputName(It.Index).ToString(), *Linked);
                }
            }
            Value += ChangedProperties(Expression, Material);
            AddRow(OutEntries, TEXT("Node:") + Expression->MaterialExpressionGuid.ToString(), DescribeExpression(Expression),
                TEXT("Node"), Value);
        }

        CollectProperties(Material, OutEntries);
    }

    static void CollectCollectionEntries(const UMaterialParameterCollection* Collection, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        // Keyed by the parameter id, so a rename reads as a change.
        for (const FCollectionScalarParameter& Param : Collection->ScalarParameters)
        {
            AddRow(OutEntries, Param.Id.ToString(), Param.ParameterName.ToString(), TEXT("Scalar"), FString::SanitizeFloat(Param.DefaultValue));
        }
        for (const FCollectionVectorParameter& Param : Collection->VectorParameters)
        {
            AddRow(OutEntries, Param.Id.ToString(), Param.ParameterName.ToString(), TEXT("Vector"), Param.DefaultValue.ToString());
        }
    }

    /** Widget Blueprint designer: one entry per widget in the tree. */
    static void CollectWidgetEntries(const UWidgetBlueprint* WidgetBlueprint, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        if (!WidgetBlueprint->WidgetTree)
        {
            return;
        }
        WidgetBlueprint->WidgetTree->ForEachWidget([&OutEntries, WidgetBlueprint](UWidget* Widget)
        {
            const FString Parent = Widget->GetParent() ? Widget->GetParent()->GetName() : FString(TEXT("(root)"));
            FString Changed = ChangedProperties(Widget, WidgetBlueprint);
            if (Widget->Slot)
            {
                const FString SlotChanged = ChangedProperties(Widget->Slot, WidgetBlueprint);
                if (!SlotChanged.IsEmpty())
                {
                    Changed += TEXT("Slot: ") + SlotChanged;
                }
            }

            FGraphXPortAssetEntry Entry;
            Entry.Key = Widget->GetFName();
            Entry.Display = Widget->GetName();
            Entry.Cells = { Widget->GetName(), Widget->GetClass()->GetName(), Parent, Changed };
            Entry.Value = FString::Printf(TEXT("%s|%s|%s"), *Entry.Cells[1], *Parent, *Changed);
            Entry.IconClass = Widget->GetClass();
            OutEntries.Add(MoveTemp(Entry));
        });
    }

    /** A level: one entry per actor the Outliner would list. */
    static void CollectActorEntries(const UWorld* World, TArray<FGraphXPortAssetEntry>& OutEntries)
    {
        const ULevel* Level = World->PersistentLevel;
        if (!Level)
        {
            return;
        }
        for (AActor* Actor : Level->Actors)
        {
            // Skip what the Outliner hides. The level script actor's name also changes between
            // saves, and its graphs are compared in the level Blueprint section anyway.
            if (!Actor || !Actor->IsListedInSceneOutliner() || Actor->IsA<AWorldSettings>() || Actor->IsA<ALevelScriptActor>()
                || Actor->IsA<ADefaultPhysicsVolume>() || Actor == Level->GetDefaultBrush())
            {
                continue;
            }

            FString ClassName = Actor->GetClass()->GetName();
            ClassName.RemoveFromEnd(TEXT("_C"));
            const AActor* AttachParent = Actor->GetAttachParentActor();
            const FTransform Transform = Actor->GetActorTransform();
            const FString TransformText = FString::Printf(TEXT("L=(%s) R=(%s) S=(%s)"),
                *Transform.GetLocation().ToCompactString(), *Transform.Rotator().ToCompactString(), *Transform.GetScale3D().ToCompactString());

            FString Changed = ChangedProperties(Actor, World);
            for (const UActorComponent* Component : Actor->GetComponents())
            {
                const FString ComponentChanged = Component ? ChangedProperties(Component, World) : FString();
                if (!ComponentChanged.IsEmpty())
                {
                    Changed += FString::Printf(TEXT("[%s] %s"), *Component->GetName(), *ComponentChanged);
                }
            }

            FGraphXPortAssetEntry Entry;
            Entry.Key = Actor->GetFName();
            Entry.Display = Actor->GetActorLabel();
            Entry.Cells = { Entry.Display, ClassName, Actor->GetFolderPath().ToString(),
                AttachParent ? AttachParent->GetActorLabel() : FString(), TransformText, Changed };
            Entry.Value = FString::Join(Entry.Cells, TEXT("|"));
            Entry.IconClass = Actor->GetClass();
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
    if (Asset->IsA<UTexture>())
    {
        return EGraphXPortAssetKind::Texture;
    }
    if (Asset->IsA<UMaterialInstance>())
    {
        return EGraphXPortAssetKind::MaterialInstance;
    }
    if (Asset->IsA<UMaterial>())
    {
        return EGraphXPortAssetKind::Material;
    }
    if (Asset->IsA<UMaterialParameterCollection>())
    {
        return EGraphXPortAssetKind::ParameterCollection;
    }
    // Only the package's main asset (named after the package) falls back to a property
    // list; generated classes, metadata and other package objects stay unsupported.
    if (!Asset->IsA<UClass>() && Asset->GetName() == FPackageName::GetShortName(Asset->GetOutermost()->GetName()))
    {
        return EGraphXPortAssetKind::Properties;
    }

    return EGraphXPortAssetKind::Unsupported;
}

int32 FGraphXPortAssetDiff::KindPriority(EGraphXPortAssetKind Kind)
{
    switch (Kind)
    {
    case EGraphXPortAssetKind::None:
    case EGraphXPortAssetKind::Unsupported:
        return 0;
    case EGraphXPortAssetKind::Properties:
        return 1;
    default:
        return 2;
    }
}

bool FGraphXPortAssetDiff::IsEntryBased(EGraphXPortAssetKind Kind)
{
    return Kind == EGraphXPortAssetKind::Enum
        || Kind == EGraphXPortAssetKind::Struct
        || Kind == EGraphXPortAssetKind::DataTable
        || Kind == EGraphXPortAssetKind::Texture
        || Kind == EGraphXPortAssetKind::MaterialInstance
        || Kind == EGraphXPortAssetKind::Material
        || Kind == EGraphXPortAssetKind::ParameterCollection
        || Kind == EGraphXPortAssetKind::Properties;
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
    case EGraphXPortAssetKind::MaterialInstance:
        return LOCTEXT("EntriesMaterialInstance", "Parameters & Properties");
    case EGraphXPortAssetKind::Material:
        return LOCTEXT("EntriesMaterial", "Nodes & Properties");
    case EGraphXPortAssetKind::ParameterCollection:
        return LOCTEXT("EntriesCollection", "Parameters");
    case EGraphXPortAssetKind::Texture:
    case EGraphXPortAssetKind::Properties:
        return LOCTEXT("EntriesProperties", "Properties");
    case EGraphXPortAssetKind::Blueprint:
        return LOCTEXT("EntriesDesigner", "Designer");
    case EGraphXPortAssetKind::LevelScript:
        return LOCTEXT("EntriesActors", "Actors");
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

    if (Asset && Asset->IsA<UWorld>())
    {
        OutColumns = { TEXT("Actor"), TEXT("Class"), TEXT("Folder"), TEXT("Attached to"), TEXT("Transform"), TEXT("Changed properties") };
        return true;
    }

    if (Asset && Asset->IsA<UWidgetBlueprint>())
    {
        OutColumns = { TEXT("Widget"), TEXT("Class"), TEXT("Parent"), TEXT("Changed properties") };
        return true;
    }

    if (IsEntryBased(DetectKind(Asset)))
    {
        OutColumns = { TEXT("Name"), TEXT("Group"), TEXT("Value") };
        return true;
    }

    return false;
}

bool FGraphXPortAssetDiff::RenderThumbnail(UObject* Asset, bool bIsDiffRevision, TArray<uint8>& OutBGRA, int32& OutWidth, int32& OutHeight)
{
    OutBGRA.Reset();
    if (!Asset)
    {
        return false;
    }

    if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset))
    {
        if (bIsDiffRevision)
        {
            Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
        }
        // A live parent may still be compiling in the background; the thumbnail would
        // then show the default material instead.
        if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
        {
            Resource->FinishCompilation();
        }
    }
    FAssetCompilingManager::Get().FinishAllCompilation();

    // A material's first render after loading still draws the default material (seen with
    // material instances), so materials get one throwaway render first.
    FObjectThumbnail Thumbnail;
    for (int32 Render = Asset->IsA<UMaterialInterface>() ? 0 : 1; Render < 2; ++Render)
    {
        ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
    }
    OutBGRA = Thumbnail.GetUncompressedImageData();
    OutWidth = Thumbnail.GetImageWidth();
    OutHeight = Thumbnail.GetImageHeight();
    // Thumbnails come back with zero alpha everywhere.
    for (int32 Index = 3; Index < OutBGRA.Num(); Index += 4)
    {
        OutBGRA[Index] = 255;
    }
    return OutWidth > 0 && OutBGRA.Num() > 0;
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
    if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset))
    {
        CollectWidgetEntries(WidgetBlueprint, OutEntries);
        return true;
    }
    if (const UWorld* World = Cast<UWorld>(Asset))
    {
        CollectActorEntries(World, OutEntries);
        return true;
    }
    if (const UTexture* Texture = Cast<UTexture>(Asset))
    {
        CollectTextureEntries(Texture, OutEntries);
        return true;
    }
    if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset))
    {
        CollectMaterialInstanceEntries(Instance, OutEntries);
        return true;
    }
    if (const UMaterial* Material = Cast<UMaterial>(Asset))
    {
        // Read-only use; the accessors for expression inputs are just not const.
        CollectMaterialEntries(const_cast<UMaterial*>(Material), OutEntries);
        return true;
    }
    if (const UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Asset))
    {
        CollectCollectionEntries(Collection, OutEntries);
        return true;
    }
    if (DetectKind(Asset) == EGraphXPortAssetKind::Properties)
    {
        CollectProperties(Asset, OutEntries);
        return true;
    }

    return false;
}

#undef LOCTEXT_NAMESPACE
