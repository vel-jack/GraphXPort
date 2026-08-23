#pragma once

#include "CoreMinimal.h"
#include "GraphXPortDocument.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

class FGraphXPortSerializer
{
public:
    static bool BuildSelection(
        const UBlueprint* Blueprint,
        const UEdGraph* Graph,
        const TArray<const UEdGraphNode*>& Nodes,
        FGraphXPortDocument& OutDocument,
        FString& OutError);

    static bool BuildGraph(
        const UBlueprint* Blueprint,
        const UEdGraph* Graph,
        FGraphXPortDocument& OutDocument,
        FString& OutError);
};
