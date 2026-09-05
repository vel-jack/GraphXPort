#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

// Pure layout computation for the manual "Beautify Selection" command. Does not touch
// live graph state - it only reads pins/connections among the given nodes and returns
// where they should go. The caller (GraphXPortMenu) is responsible for applying the
// result inside an undo transaction.
class FGraphXPortBeautifier
{
public:
    // Lays out SelectedNodes left-to-right in execution-flow layers (topological columns),
    // with pure/data-only nodes placed one layer before whichever node consumes them.
    // Column widths and row heights are sized from each node's estimated pin footprint
    // (there is no rendered widget size available for a node that isn't necessarily on
    // screen), not a fixed cell size, so real nodes of varying width/height don't overlap.
    // Edges to nodes outside SelectedNodes are treated as selection boundaries and do not
    // affect layering. Returns false (OutPositions left empty) when fewer than two nodes
    // are given, since there is nothing meaningful to arrange.
    //
    // PreferredAnchor, when part of SelectedNodes, keeps its own original position exactly
    // and every other node is placed relative to it - so the layout always starts from a
    // specific node (e.g. the one the user right-clicked) rather than an arbitrary bounding
    // box, and everything downstream of it reads left-to-right from that fixed point. When
    // null or not found, the earliest node in the layout (smallest layer/row) is used instead.
    static bool ComputeLayout(const TArray<UEdGraphNode*>& SelectedNodes, TMap<UEdGraphNode*, FVector2D>& OutPositions,
        UEdGraphNode* PreferredAnchor = nullptr);

    static constexpr int32 MaxDepth = 128;
};
