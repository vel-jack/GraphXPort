#include "GraphXPortBeautifier.h"

#include "Algo/Sort.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

namespace
{
    constexpr float ColumnGutter = 80.f;
    constexpr float RowGutter = 40.f;

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    bool HasExecPin(const UEdGraphNode* Node)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
            if (IsExecPin(Pin)) return true;
        return false;
    }

    // Node->Pins already reflects pin declaration order, which is what the ASCII formatter
    // uses to keep Branch/Sequence/Switch/loop output order deterministic and meaningful;
    // Beautify follows the same rule so layering agrees with that output order.
    TArray<UEdGraphPin*> OrderedOutputExecPins(const UEdGraphNode* Node)
    {
        TArray<UEdGraphPin*> Result;
        for (UEdGraphPin* Pin : Node->Pins)
            if (Pin != nullptr && Pin->Direction == EGPD_Output && IsExecPin(Pin))
                Result.Add(Pin);
        return Result;
    }

    // There is no rendered widget size available for a node that may not currently be on
    // screen, so width/height are estimated from pin counts and label length - the same
    // inputs the real Blueprint node widget sizes itself from. Approximate, but it scales
    // with what actually makes a node wide (long pin names) or tall (many pins), which a
    // fixed grid cell cannot.
    float EstimateNodeWidth(const UEdGraphNode* Node)
    {
        int32 MaxLabelLen = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Len();
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin != nullptr && !Pin->bHidden)
            {
                MaxLabelLen = FMath::Max(MaxLabelLen, Pin->PinName.ToString().Len());
            }
        }
        return FMath::Clamp(180.f + MaxLabelLen * 7.f, 180.f, 640.f);
    }

    float EstimateNodeHeight(const UEdGraphNode* Node)
    {
        int32 InputCount = 0;
        int32 OutputCount = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin == nullptr || Pin->bHidden) continue;
            if (Pin->Direction == EGPD_Input) ++InputCount; else ++OutputCount;
        }
        // Input and output pins stack in independent columns inside a node, so height
        // tracks whichever side has more rows, not the pin total.
        const int32 RowCount = FMath::Max(FMath::Max(InputCount, OutputCount), 1);
        return 40.f + RowCount * 26.f;
    }

    // Simple skyline packer: nodes are placed left-to-right in known columns, but rows
    // are assigned greedily as clusters are built, so two unrelated nodes can still end
    // up wanting the same space. Occupied tracks every rect placed so far; Place() slides
    // a candidate straight down (never sideways, so it stays in its intended column)
    // until it clears every previously placed rect, then records it. This is what
    // actually guarantees no overlap, rather than trusting the grid arithmetic alone.
    struct FRectPacker
    {
        struct FRect { float X0, Y0, X1, Y1; };
        TArray<FRect> Occupied;

        static bool Overlaps(const FRect& A, const FRect& B)
        {
            return A.X0 < B.X1 && B.X0 < A.X1 && A.Y0 < B.Y1 && B.Y0 < A.Y1;
        }

        float Place(float X0, float X1, float DesiredY0, float Height)
        {
            float Y0 = DesiredY0;
            bool bMoved = true;
            while (bMoved)
            {
                bMoved = false;
                for (const FRect& R : Occupied)
                {
                    const FRect Candidate{X0, Y0, X1, Y0 + Height};
                    if (Overlaps(Candidate, R))
                    {
                        Y0 = R.Y1 + RowGutter;
                        bMoved = true;
                    }
                }
            }
            Occupied.Add(FRect{X0, Y0, X1, Y0 + Height});
            return Y0;
        }

        void Reserve(float X0, float Y0, float X1, float Y1)
        {
            Occupied.Add(FRect{X0, Y0, X1, Y1});
        }
    };
}

bool FGraphXPortBeautifier::ComputeLayout(const TArray<UEdGraphNode*>& SelectedNodes, TMap<UEdGraphNode*, FVector2D>& OutPositions,
    UEdGraphNode* PreferredAnchor)
{
    OutPositions.Reset();

    TArray<UEdGraphNode*> Nodes;
    TSet<UEdGraphNode*> NodeSet;
    for (UEdGraphNode* Node : SelectedNodes)
    {
        if (Node != nullptr && !NodeSet.Contains(Node))
        {
            Nodes.Add(Node);
            NodeSet.Add(Node);
        }
    }
    if (Nodes.Num() < 2) return false;

    // --- Phase 1: layer/row the execution "spine" (nodes with an exec pin). Pure/data
    // nodes are handled separately in Phase 3 - they are not part of this grid at all,
    // which is what lets each one be placed next to its own consumer instead of sharing
    // one distant global column with every other pure node in the selection.
    TMap<UEdGraphNode*, int32> Layer;
    TSet<UEdGraphNode*> Visiting;

    // Longest-path layering: a node's layer is the max over its in-selection predecessors'
    // layer + 1. Visiting guards against re-entering a node already on the current DFS
    // stack (execution cycles), and MaxDepth is a hard backstop against runaway recursion,
    // mirroring the guards FGraphXPortAsciiFormatter uses for the same failure modes.
    TFunction<void(UEdGraphNode*, int32, int32)> AssignLayer;
    AssignLayer = [&](UEdGraphNode* Node, const int32 IncomingLayer, const int32 Depth)
    {
        const int32* Existing = Layer.Find(Node);
        const int32 NewLayer = FMath::Max(Existing ? *Existing : 0, IncomingLayer);
        Layer.FindOrAdd(Node) = NewLayer;

        if (Depth > FGraphXPortBeautifier::MaxDepth || Visiting.Contains(Node))
        {
            return;
        }

        Visiting.Add(Node);
        for (UEdGraphPin* Pin : OrderedOutputExecPins(Node))
        {
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Target = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                if (Target != nullptr && NodeSet.Contains(Target))
                {
                    AssignLayer(Target, Layer[Node] + 1, Depth + 1);
                }
            }
        }
        Visiting.Remove(Node);
    };

    // Root discovery mirrors the ASCII formatter's multi-pass approach: nodes with no
    // incoming exec edge from within the selection are natural roots; anything left over
    // (rootless cycles) still needs its own root pass so it gets laid out.
    TSet<UEdGraphNode*> HasIncomingExecFromSelection;
    for (UEdGraphNode* Node : Nodes)
        for (UEdGraphPin* Pin : OrderedOutputExecPins(Node))
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Target = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                if (Target != nullptr && NodeSet.Contains(Target))
                    HasIncomingExecFromSelection.Add(Target);
            }

    for (UEdGraphNode* Node : Nodes)
        if (HasExecPin(Node) && !HasIncomingExecFromSelection.Contains(Node))
            AssignLayer(Node, 0, 0);
    for (UEdGraphNode* Node : Nodes)
        if (HasExecPin(Node) && !Layer.Contains(Node))
            AssignLayer(Node, 0, 0);

    TArray<UEdGraphNode*> SpineOrder;
    for (UEdGraphNode* Node : Nodes)
        if (HasExecPin(Node)) SpineOrder.Add(Node);

    if (!SpineOrder.IsEmpty())
    {
        // In/out exec degree, and each node's sole exec predecessor (when it has exactly
        // one). Used to straighten simple A -> B pass-through links: if a node has exactly
        // one incoming exec edge and that source has exactly one outgoing exec edge, they
        // are a straight chain link and should share a row so the connecting wire is
        // horizontal instead of arcing to a different row.
        TMap<int32, TArray<UEdGraphNode*>> ByLayer;
        for (const TPair<UEdGraphNode*, int32>& Pair : Layer) ByLayer.FindOrAdd(Pair.Value).Add(Pair.Key);
        for (TPair<int32, TArray<UEdGraphNode*>>& Pair : ByLayer)
        {
            Pair.Value.StableSort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosY < B.NodePosY; });
        }

        TMap<UEdGraphNode*, int32> OutDegree;
        TMap<UEdGraphNode*, UEdGraphNode*> SoleSource;
        {
            TMap<UEdGraphNode*, int32> InDegree;
            for (UEdGraphNode* Node : SpineOrder)
            {
                for (UEdGraphPin* Pin : OrderedOutputExecPins(Node))
                {
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Target = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                        if (Target == nullptr || !NodeSet.Contains(Target)) continue;
                        ++OutDegree.FindOrAdd(Node);
                        int32& TargetInDegree = InDegree.FindOrAdd(Target);
                        ++TargetInDegree;
                        if (TargetInDegree == 1) SoleSource.Add(Target, Node);
                        else SoleSource.Remove(Target);
                    }
                }
            }
        }

        // Row assignment: process layers in increasing order so an aligned node's source
        // row is always already assigned. A node inherits its source's row when the two
        // form a straight 1-out/1-in chain link; otherwise (branch arms, merges, roots) it
        // takes the first free row in its own layer. Collisions (two nodes wanting the
        // same inherited row) resolve to the next free row instead.
        TArray<int32> LayerKeys;
        ByLayer.GetKeys(LayerKeys);
        LayerKeys.Sort();

        TMap<UEdGraphNode*, int32> Row;
        TMap<int32, TSet<int32>> UsedRowsByLayer;
        for (const int32 LayerKey : LayerKeys)
        {
            for (UEdGraphNode* Node : ByLayer[LayerKey])
            {
                int32 DesiredRow = 0;
                if (UEdGraphNode* const* Predecessor = SoleSource.Find(Node))
                {
                    const int32* PredecessorOutDegree = OutDegree.Find(*Predecessor);
                    const int32* PredecessorLayer = Layer.Find(*Predecessor);
                    const int32* PredecessorRow = Row.Find(*Predecessor);
                    if (PredecessorOutDegree != nullptr && *PredecessorOutDegree == 1
                        && PredecessorLayer != nullptr && *PredecessorLayer < LayerKey
                        && PredecessorRow != nullptr)
                    {
                        DesiredRow = *PredecessorRow;
                    }
                }
                TSet<int32>& UsedRows = UsedRowsByLayer.FindOrAdd(LayerKey);
                int32 CandidateRow = DesiredRow;
                while (UsedRows.Contains(CandidateRow)) ++CandidateRow;
                Row.Add(Node, CandidateRow);
                UsedRows.Add(CandidateRow);
            }
        }

        // Grid sizing: each column is as wide as its widest node plus a gutter, each row
        // as tall as its tallest node plus a gutter, and coordinates are the cumulative
        // sum of prior columns/rows - so a shared row index maps to the same Y in every
        // column (straight chains stay horizontal) while columns/rows both grow to fit
        // what's actually placed in them.
        TArray<int32> RowKeysSet;
        for (const TPair<UEdGraphNode*, int32>& Pair : Row) RowKeysSet.AddUnique(Pair.Value);
        RowKeysSet.Sort();

        TMap<int32, float> ColumnWidth;
        for (const int32 LayerKey : LayerKeys)
        {
            float Widest = 0.f;
            for (const UEdGraphNode* Node : ByLayer[LayerKey]) Widest = FMath::Max(Widest, EstimateNodeWidth(Node));
            ColumnWidth.Add(LayerKey, Widest);
        }
        TMap<int32, float> RowHeight;
        for (const int32 RowKey : RowKeysSet)
        {
            float Tallest = 0.f;
            for (UEdGraphNode* Node : SpineOrder)
                if (Row[Node] == RowKey) Tallest = FMath::Max(Tallest, EstimateNodeHeight(Node));
            RowHeight.Add(RowKey, Tallest);
        }

        TMap<int32, float> ColumnX;
        float CumulativeX = 0.f;
        for (const int32 LayerKey : LayerKeys)
        {
            ColumnX.Add(LayerKey, CumulativeX);
            CumulativeX += ColumnWidth[LayerKey] + ColumnGutter;
        }
        TMap<int32, float> RowY;
        float CumulativeY = 0.f;
        for (const int32 RowKey : RowKeysSet)
        {
            RowY.Add(RowKey, CumulativeY);
            CumulativeY += RowHeight[RowKey] + RowGutter;
        }

        // Anchor the whole layout to one specific node instead of an arbitrary bounding
        // box: whichever node is chosen as the anchor keeps its own original position
        // exactly, and every other node's raw grid coordinate is expressed as an offset
        // from the anchor's raw grid coordinate. That is what makes the anchor node the
        // fixed starting point everything else reads left-to-right from - later layers
        // land to its right, earlier layers (and its own pure inputs) to its left.
        UEdGraphNode* AnchorNode = (PreferredAnchor != nullptr && SpineOrder.Contains(PreferredAnchor))
            ? PreferredAnchor : nullptr;
        if (AnchorNode == nullptr)
        {
            for (UEdGraphNode* Node : SpineOrder)
            {
                if (AnchorNode == nullptr
                    || Layer[Node] < Layer[AnchorNode]
                    || (Layer[Node] == Layer[AnchorNode] && Row[Node] < Row[AnchorNode]))
                {
                    AnchorNode = Node;
                }
            }
        }
        const float AnchorRawX = ColumnX[Layer[AnchorNode]];
        const float AnchorRawY = RowY[Row[AnchorNode]];

        for (UEdGraphNode* Node : SpineOrder)
        {
            OutPositions.Add(Node, FVector2D(
                AnchorNode->NodePosX + (ColumnX[Layer[Node]] - AnchorRawX),
                AnchorNode->NodePosY + (RowY[Row[Node]] - AnchorRawY)));
        }
    }
    else
    {
        // Pure-only selection: there is no spine to anchor against, so fall through to
        // Phase 3's orphan handling for every node.
    }

    // --- Phase 2: claim each pure/data-only node to the nearest spine node that actually
    // consumes it, walking backward from every spine node's own data input pins (and, from
    // there, into each claimed pure node's inputs in turn) so multi-hop pure chains resolve
    // correctly. First spine processed claims a shared pure node, so it is not placed twice.
    // A spine node with an exec pin is never claimed - it already has a Phase 1 position.
    TMap<UEdGraphNode*, UEdGraphNode*> ClusterOwner;
    TMap<UEdGraphNode*, int32> ClusterDepth;
    {
        TArray<UEdGraphNode*> ClaimOrder = SpineOrder;
        Algo::Sort(ClaimOrder, [&Layer](UEdGraphNode* A, UEdGraphNode* B) { return Layer[A] < Layer[B]; });

        TSet<UEdGraphNode*> Claimed;
        TFunction<void(UEdGraphNode*, UEdGraphNode*, int32, int32)> ClaimBackward;
        ClaimBackward = [&](UEdGraphNode* Node, UEdGraphNode* Owner, const int32 Depth, const int32 RecursionGuard)
        {
            if (RecursionGuard > FGraphXPortBeautifier::MaxDepth) return;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr || Pin->Direction != EGPD_Input || IsExecPin(Pin)) continue;
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Source = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                    if (Source == nullptr || !NodeSet.Contains(Source) || HasExecPin(Source) || Claimed.Contains(Source)) continue;
                    Claimed.Add(Source);
                    ClusterOwner.Add(Source, Owner);
                    ClusterDepth.Add(Source, Depth);
                    ClaimBackward(Source, Owner, Depth + 1, RecursionGuard + 1);
                }
            }
        };
        for (UEdGraphNode* Spine : ClaimOrder)
        {
            ClaimBackward(Spine, Spine, 1, 0);
        }
    }

    // --- Phase 3: place pure clusters immediately left of (and top-aligned with) their
    // owning spine node, one column per hop of distance from it, with siblings at the
    // same hop stacked vertically. Any node nothing claimed (an orphan pure node, or every
    // node when the selection has no spine at all) stacks in its own column to the left of
    // everything else. FRectPacker slides a candidate straight down on real collision
    // against anything already placed (spine or pure), which is what actually prevents
    // overlap rather than trusting the grid arithmetic alone.
    FRectPacker Packer;
    for (const TPair<UEdGraphNode*, FVector2D>& Pair : OutPositions)
    {
        Packer.Reserve(Pair.Value.X, Pair.Value.Y, Pair.Value.X + EstimateNodeWidth(Pair.Key), Pair.Value.Y + EstimateNodeHeight(Pair.Key));
    }

    TMap<UEdGraphNode*, TMap<int32, TArray<UEdGraphNode*>>> ClustersByOwnerDepth;
    TArray<UEdGraphNode*> Orphans;
    for (UEdGraphNode* Node : Nodes)
    {
        if (HasExecPin(Node)) continue;
        if (UEdGraphNode* const* OwnerPtr = ClusterOwner.Find(Node))
        {
            ClustersByOwnerDepth.FindOrAdd(*OwnerPtr).FindOrAdd(ClusterDepth[Node]).Add(Node);
        }
        else
        {
            Orphans.Add(Node);
        }
    }

    TArray<UEdGraphNode*> OwnerOrder;
    ClustersByOwnerDepth.GetKeys(OwnerOrder);
    Algo::Sort(OwnerOrder, [&Layer](UEdGraphNode* A, UEdGraphNode* B) { return Layer[A] < Layer[B]; });

    for (UEdGraphNode* Owner : OwnerOrder)
    {
        const FVector2D OwnerPos = OutPositions[Owner];
        TArray<int32> Depths;
        ClustersByOwnerDepth[Owner].GetKeys(Depths);
        Depths.Sort();

        float CumulativeOffset = 0.f;
        for (const int32 Depth : Depths)
        {
            TArray<UEdGraphNode*>& Group = ClustersByOwnerDepth[Owner][Depth];
            Algo::Sort(Group, [](UEdGraphNode* A, UEdGraphNode* B) { return A->NodePosY < B->NodePosY; });

            float ColumnWidthAtDepth = 0.f;
            for (UEdGraphNode* Node : Group) ColumnWidthAtDepth = FMath::Max(ColumnWidthAtDepth, EstimateNodeWidth(Node));

            const float ColumnRight = OwnerPos.X - ColumnGutter - CumulativeOffset;
            const float ColumnLeft = ColumnRight - ColumnWidthAtDepth;

            float NextY = OwnerPos.Y;
            for (UEdGraphNode* Node : Group)
            {
                const float Height = EstimateNodeHeight(Node);
                const float FinalY = Packer.Place(ColumnLeft, ColumnLeft + ColumnWidthAtDepth, NextY, Height);
                OutPositions.Add(Node, FVector2D(ColumnLeft, FinalY));
                NextY = FinalY + Height + RowGutter;
            }
            CumulativeOffset += ColumnWidthAtDepth + ColumnGutter;
        }
    }

    if (!Orphans.IsEmpty())
    {
        Algo::Sort(Orphans, [](UEdGraphNode* A, UEdGraphNode* B) { return A->NodePosY < B->NodePosY; });

        float LeftmostX = TNumericLimits<float>::Max();
        float TopmostY = TNumericLimits<float>::Max();
        if (OutPositions.IsEmpty())
        {
            // No spine at all (a pure-only selection): anchor at PreferredAnchor's own
            // original position when it's part of this selection, otherwise near the
            // selection's own bounding box instead of snapping to the graph origin.
            if (PreferredAnchor != nullptr && NodeSet.Contains(PreferredAnchor))
            {
                LeftmostX = static_cast<float>(PreferredAnchor->NodePosX);
                TopmostY = static_cast<float>(PreferredAnchor->NodePosY);
            }
            else
            {
                for (const UEdGraphNode* Node : Orphans)
                {
                    LeftmostX = FMath::Min(LeftmostX, static_cast<float>(Node->NodePosX));
                    TopmostY = FMath::Min(TopmostY, static_cast<float>(Node->NodePosY));
                }
            }
        }
        else
        {
            for (const TPair<UEdGraphNode*, FVector2D>& Pair : OutPositions)
            {
                LeftmostX = FMath::Min(LeftmostX, Pair.Value.X);
                TopmostY = FMath::Min(TopmostY, Pair.Value.Y);
            }
        }

        float OrphanWidth = 0.f;
        for (UEdGraphNode* Node : Orphans) OrphanWidth = FMath::Max(OrphanWidth, EstimateNodeWidth(Node));
        const float ColumnLeft = OutPositions.IsEmpty() ? LeftmostX : LeftmostX - ColumnGutter - OrphanWidth;

        float NextY = TopmostY;
        for (UEdGraphNode* Node : Orphans)
        {
            const float Height = EstimateNodeHeight(Node);
            const float FinalY = Packer.Place(ColumnLeft, ColumnLeft + OrphanWidth, NextY, Height);
            OutPositions.Add(Node, FVector2D(ColumnLeft, FinalY));
            NextY = FinalY + Height + RowGutter;
        }
    }

    return true;
}
