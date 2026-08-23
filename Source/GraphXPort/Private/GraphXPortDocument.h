#pragma once

#include "CoreMinimal.h"

struct FGraphXPortPin
{
    int32 NodePinOrder = INDEX_NONE;
    FString Id;
    FString Name;
    FString Direction;
    FString Category;
    FString SubCategory;
    FString ContainerType;
    FString TypeObjectPath;
    FString DefaultValue;
    FString DefaultObjectPath;
    FString DefaultText;
    bool bHidden = false;
    bool bAdvancedView = false;
    bool bOrphaned = false;
};

struct FGraphXPortNode
{
    FString Id;
    FString Name;
    FString ClassPath;
    FString Title;
    FString Comment;
    FString MemberName;
    FString MemberParentPath;
    FString MacroGraphPath;
    int32 PositionX = 0;
    int32 PositionY = 0;
    TArray<FGraphXPortPin> Pins;
};

struct FGraphXPortEdge
{
    FString SourceNodeId;
    FString SourcePinId;
    FString SourcePinName;
    FString TargetNodeId;
    FString TargetPinId;
    FString TargetPinName;
    FString Kind;
};

struct FGraphXPortExternalLink
{
    FString LocalNodeId;
    FString LocalPinId;
    FString LocalPinName;
    FString Direction;
    FString ExternalNodeId;
    FString ExternalNodeTitle;
    FString ExternalNodeClassPath;
    FString ExternalPinId;
    FString ExternalPinName;
};

struct FGraphXPortDocument
{
    int32 FormatVersion = 1;
    FString BlueprintPath;
    FString BlueprintName;
    FString GraphName;
    FString GraphId;
    FString Scope;
    TArray<FGraphXPortNode> Nodes;
    TArray<FGraphXPortEdge> Edges;
    TArray<FGraphXPortExternalLink> ExternalLinks;
};
