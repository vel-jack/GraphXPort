#pragma once

#include "CoreMinimal.h"
#include "GraphXPortFormatter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UToolMenu;

class FGraphXPortMenu
{
public:
    static void AddContextSection(UToolMenu* Menu);
    static void PopulateSubmenu(UToolMenu* Menu);

private:
    static void Export(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* ClickedNode,
        EGraphXPortFormat Format, bool bEntireGraph, bool bSaveFile);
};
