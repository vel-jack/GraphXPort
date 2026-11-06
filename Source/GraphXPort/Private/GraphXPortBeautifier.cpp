#include "GraphXPortBeautifier.h"

#include "Algo/Sort.h"
#include "Algo/StableSort.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_VariableGet.h"

namespace
{
    constexpr float ColumnGutter = 50.f;
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
    constexpr float PinRowHeight = 26.f;
    constexpr float ExecRowClearance = 52.f;

    // Unconnected data inputs draw an inline default widget: a combo box for enums, a text
    // box for strings/names/text, a small box or checkbox otherwise. Combos and text boxes
    // are both wider and taller than a plain pin row.
    bool IsComboPin(const UEdGraphPin* Pin)
    {
        return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && Pin->PinType.PinSubCategoryObject.IsValid();
    }

    float InlineWidgetWidth(const UEdGraphPin* Pin)
    {
        if (IsExecPin(Pin) || !Pin->LinkedTo.IsEmpty()) return 0.f;
        if (IsComboPin(Pin)) return 170.f;
        const FName Category = Pin->PinType.PinCategory;
        if (Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Name || Category == UEdGraphSchema_K2::PC_Text) return 110.f;
        return 50.f;
    }

    float InlineRowHeight(const UEdGraphPin* Pin)
    {
        if (IsExecPin(Pin) || !Pin->LinkedTo.IsEmpty()) return PinRowHeight;
        return (IsComboPin(Pin) || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String) ? 44.f : PinRowHeight;
    }

    // Math operators (compact) and variable getters draw without the usual title bar and
    // without pin labels, so they are far smaller than a function-call node.
    bool IsHeaderless(const UEdGraphNode* Node)
    {
        const UK2Node* K2Node = Cast<UK2Node>(Node);
        return (K2Node != nullptr && K2Node->ShouldDrawCompact()) || Cast<UK2Node_VariableGet>(Node) != nullptr;
    }

    TArray<const UEdGraphPin*> VisiblePins(const UEdGraphNode* Node, const EEdGraphPinDirection Direction)
    {
        TArray<const UEdGraphPin*> Result;
        for (const UEdGraphPin* Pin : Node->Pins)
            if (Pin != nullptr && !Pin->bHidden && Pin->Direction == Direction) Result.Add(Pin);
        return Result;
    }

    // Vertical centre of a pin relative to the node's top, so a source's output wire can be
    // levelled with the consumer's input pin it feeds.
    float PinCenterY(const UEdGraphNode* Node, const UEdGraphPin* Pin)
    {
        const int32 Index = FMath::Max(0, VisiblePins(Node, Pin->Direction).IndexOfByKey(Pin));
        return (IsHeaderless(Node) ? 4.f : 34.f) + Index * PinRowHeight + PinRowHeight * 0.5f;
    }

    // There is no rendered widget size available for a node that may not currently be on
    // screen, so size is estimated from what the real widget lays out: a title bar, then
    // rows pairing the i-th input with the i-th output, each row as wide as its two labels
    // (plus an inline default-value box for unconnected inputs).
    float EstimateNodeWidth(const UEdGraphNode* Node)
    {
        const int32 TitleLen = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Len();
        if (IsHeaderless(Node))
        {
            // Compact nodes draw their title as a big watermark ("FIND" is ~160 wide), a
            // lone operator symbol ("*") is ~80.
            if (Cast<UK2Node_VariableGet>(Node) != nullptr) return FMath::Clamp(50.f + TitleLen * 8.f, 80.f, 400.f);
            return FMath::Clamp(60.f + TitleLen * 22.f, 80.f, 400.f);
        }

        const TArray<const UEdGraphPin*> Inputs = VisiblePins(Node, EGPD_Input);
        const TArray<const UEdGraphPin*> Outputs = VisiblePins(Node, EGPD_Output);
        float WidestRow = 0.f;
        for (int32 i = 0; i < FMath::Max(Inputs.Num(), Outputs.Num()); ++i)
        {
            float RowWidth = 0.f;
            if (Inputs.IsValidIndex(i))
            {
                RowWidth += Inputs[i]->GetDisplayName().ToString().Len() * 7.f;
                RowWidth += InlineWidgetWidth(Inputs[i]);
            }
            if (Outputs.IsValidIndex(i)) RowWidth += Outputs[i]->GetDisplayName().ToString().Len() * 7.f;
            WidestRow = FMath::Max(WidestRow, RowWidth);
        }
        const float Gap = (!Inputs.IsEmpty() && !Outputs.IsEmpty()) ? 24.f : 0.f;
        return FMath::Clamp(FMath::Max(60.f + WidestRow + Gap, 50.f + TitleLen * 8.f), 120.f, 640.f);
    }

    float EstimateNodeHeight(const UEdGraphNode* Node)
    {
        // Input and output pins stack in independent columns inside a node, so height
        // tracks whichever side has more rows, not the pin total.
        const TArray<const UEdGraphPin*> Inputs = VisiblePins(Node, EGPD_Input);
        const int32 RowCount = FMath::Max(1, FMath::Max(Inputs.Num(), VisiblePins(Node, EGPD_Output).Num()));
        float Rows = 0.f;
        for (int32 i = 0; i < RowCount; ++i)
        {
            Rows += Inputs.IsValidIndex(i) ? InlineRowHeight(Inputs[i]) : PinRowHeight;
        }
        // A compiler warning/error draws a banner under the node.
        return (IsHeaderless(Node) ? 8.f : 48.f) + Rows + (Node->bHasCompilerMessage ? 28.f : 0.f);
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
            // Estimates are approximate (and nodes can grow a warning/error badge), so keep a
            // safety margin around every rect instead of requiring exact contact.
            constexpr float Margin = 24.f;
            return A.X0 < B.X1 + Margin && B.X0 < A.X1 + Margin && A.Y0 < B.Y1 + Margin && B.Y0 < A.Y1 + Margin;
        }

        // Picks the free Y (>= MinY) closest to DesiredY0 in either direction: just below or
        // just above any rect in the way. Staying near the desired Y is what keeps a wire
        // straight when something else happens to occupy the exact spot. The unbounded
        // bottom-most candidate is always free, so a result always exists.
        float Place(float X0, float X1, float DesiredY0, float Height, float MinY = -TNumericLimits<float>::Max())
        {
            constexpr float Gap = 25.f; // just past Overlaps()'s margin
            TArray<float, TInlineAllocator<16>> Candidates;
            Candidates.Add(FMath::Max(DesiredY0, MinY));
            for (const FRect& R : Occupied)
            {
                Candidates.Add(R.Y1 + Gap);
                Candidates.Add(R.Y0 - Gap - Height);
            }

            float Best = 0.f;
            float BestDistance = TNumericLimits<float>::Max();
            for (const float Y0 : Candidates)
            {
                if (Y0 < MinY) continue;
                const FRect Candidate{X0, Y0, X1, Y0 + Height};
                bool bFree = true;
                for (const FRect& R : Occupied)
                    if (Overlaps(Candidate, R)) { bFree = false; break; }
                const float Distance = FMath::Abs(Y0 - DesiredY0);
                if (bFree && Distance < BestDistance) { Best = Y0; BestDistance = Distance; }
            }
            Occupied.Add(FRect{X0, Best, X1, Best + Height});
            return Best;
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

    // --- Phase 2 (runs before the grid is sized so each layer can reserve room for its
    // pure nodes): claim each pure/data-only node to the nearest spine node that consumes
    // it, walking backward from every spine node's data input pins (and from there into
    // each claimed pure node's inputs) so multi-hop pure chains resolve. First spine
    // processed claims a shared pure node, so it is not placed twice. Each claim remembers
    // the exact consumer pin / source pin so Phase 3 can level the wire between them.
    struct FClaim
    {
        UEdGraphNode* Owner = nullptr;
        const UEdGraphNode* Consumer = nullptr;
        const UEdGraphPin* ConsumerPin = nullptr;
        const UEdGraphPin* SourcePin = nullptr;
        int32 Depth = 0;
    };
    TMap<UEdGraphNode*, FClaim> Claims;
    {
        TArray<UEdGraphNode*> ClaimOrder = SpineOrder;
        Algo::StableSort(ClaimOrder, [&Layer](UEdGraphNode* A, UEdGraphNode* B) { return Layer[A] < Layer[B]; });

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
                    if (Source == nullptr || !NodeSet.Contains(Source) || HasExecPin(Source) || Claims.Contains(Source)) continue;
                    Claims.Add(Source, FClaim{Owner, Node, Pin, Linked, Depth});
                    ClaimBackward(Source, Owner, Depth + 1, RecursionGuard + 1);
                }
            }
        };
        for (UEdGraphNode* Spine : ClaimOrder)
        {
            ClaimBackward(Spine, Spine, 1, 0);
        }
    }

    // Re-home each pure node so it sits LEFT of every consumer (no wire runs backwards):
    // pick the consumer in the earliest layer, and within that layer the deepest one, then
    // go one column further left. A pure node feeding both a nearby and a distant consumer
    // therefore gets a long forward wire instead of a backward one. Fixed-point loop since a
    // consumer's own placement can change; capped for safety.
    for (int32 Pass = 0; Pass < FGraphXPortBeautifier::MaxDepth; ++Pass)
    {
        bool bChanged = false;
        for (TPair<UEdGraphNode*, FClaim>& Entry : Claims)
        {
            FClaim Best;
            int32 BestLayer = MAX_int32;
            for (const UEdGraphPin* Pin : Entry.Key->Pins)
            {
                if (Pin == nullptr || Pin->Direction != EGPD_Output || IsExecPin(Pin)) continue;
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    const UEdGraphNode* Consumer = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                    if (Consumer == nullptr || Consumer == Entry.Key || !NodeSet.Contains(const_cast<UEdGraphNode*>(Consumer))) continue;

                    UEdGraphNode* Owner;
                    int32 ConsumerDepth;
                    if (HasExecPin(Consumer)) { Owner = const_cast<UEdGraphNode*>(Consumer); ConsumerDepth = 0; }
                    else if (const FClaim* CC = Claims.Find(const_cast<UEdGraphNode*>(Consumer))) { Owner = CC->Owner; ConsumerDepth = CC->Depth; }
                    else continue;

                    const int32 OwnerLayer = Layer[Owner];
                    if (OwnerLayer < BestLayer || (OwnerLayer == BestLayer && ConsumerDepth + 1 > Best.Depth))
                    {
                        BestLayer = OwnerLayer;
                        Best = FClaim{Owner, Consumer, Linked, Pin, ConsumerDepth + 1};
                    }
                }
            }
            if (Best.Owner != nullptr && (Best.Owner != Entry.Value.Owner || Best.Depth != Entry.Value.Depth))
            {
                Entry.Value = Best;
                bChanged = true;
            }
        }
        if (!bChanged) break;
    }

    // Raw-grid x of each pure column (layer -> depth -> left edge) and the shift that maps
    // raw grid coordinates onto the anchor node's real position.
    TMap<int32, TMap<int32, float>> PureColumnLeft;
    float AnchorShiftX = 0.f;

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

        TArray<int32> LayerKeys;
        ByLayer.GetKeys(LayerKeys);
        LayerKeys.Sort();

        // Row assignment as a depth-first tree walk, so every chain stays together as one
        // contiguous band of rows (a Sequence's "Then 1" chain no longer interleaves with
        // its "Then 2" chain). A node's first not-yet-placed child continues on the node's
        // own row (straight wire); each further child (Branch's False, Sequence's later
        // pins) starts a new band below everything placed so far. A node already placed
        // (a merge point, or a cycle edge) keeps its first row. Walk order follows output
        // pin declaration order, then the original Y, so results are deterministic.
        TMap<UEdGraphNode*, int32> Row;
        int32 MaxRowUsed = -1;
        TFunction<void(UEdGraphNode*, int32, int32)> AssignRow;
        AssignRow = [&](UEdGraphNode* Node, const int32 NodeRow, const int32 Depth)
        {
            if (Row.Contains(Node)) return;
            Row.Add(Node, NodeRow);
            MaxRowUsed = FMath::Max(MaxRowUsed, NodeRow);
            if (Depth > FGraphXPortBeautifier::MaxDepth) return;

            bool bFirstChild = true;
            for (UEdGraphPin* Pin : OrderedOutputExecPins(Node))
            {
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Target = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
                    if (Target == nullptr || !NodeSet.Contains(Target) || Row.Contains(Target)) continue;
                    AssignRow(Target, bFirstChild ? NodeRow : MaxRowUsed + 1, Depth + 1);
                    bFirstChild = false;
                }
            }
        };

        TArray<UEdGraphNode*> RowRoots = SpineOrder;
        RowRoots.StableSort([&](const UEdGraphNode& A, const UEdGraphNode& B)
        {
            const bool bARoot = !HasIncomingExecFromSelection.Contains(const_cast<UEdGraphNode*>(&A));
            const bool bBRoot = !HasIncomingExecFromSelection.Contains(const_cast<UEdGraphNode*>(&B));
            if (bARoot != bBRoot) return bARoot; // true roots first; rootless cycle nodes after
            return A.NodePosY < B.NodePosY;
        });
        for (UEdGraphNode* Root : RowRoots)
        {
            AssignRow(Root, MaxRowUsed + 1, 0);
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

        // Each layer reserves a gap in front of its column for the pure columns feeding it
        // (one column per hop of depth), so pure nodes sit between spine columns instead of
        // colliding with the previous layer's nodes and being shoved far away.
        TMap<int32, TMap<int32, float>> PureWidth;
        for (const TPair<UEdGraphNode*, FClaim>& Pair : Claims)
        {
            float& Width = PureWidth.FindOrAdd(Layer[Pair.Value.Owner]).FindOrAdd(Pair.Value.Depth);
            Width = FMath::Max(Width, EstimateNodeWidth(Pair.Key));
        }

        TMap<int32, float> ColumnX;
        float CumulativeX = 0.f;
        for (const int32 LayerKey : LayerKeys)
        {
            if (const TMap<int32, float>* Widths = PureWidth.Find(LayerKey))
            {
                TArray<int32> Depths;
                Widths->GetKeys(Depths);
                Depths.Sort();
                // No horizontal room is reserved: pure columns hang off the owner's left edge
                // (depth 1 nearest, deeper hops further left) and the packer drops them into
                // free vertical space under the exec rows, which keeps the graph compact.
                float Right = CumulativeX - ColumnGutter;
                TMap<int32, float>& Lefts = PureColumnLeft.Add(LayerKey);
                for (const int32 Depth : Depths)
                {
                    Right -= (*Widths)[Depth];
                    Lefts.Add(Depth, Right);
                    Right -= ColumnGutter;
                }
            }
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
        AnchorShiftX = AnchorNode->NodePosX - AnchorRawX;

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

    // --- Phase 3: place pure nodes in the gap reserved in front of their owner's layer, one
    // column per hop of depth. Each node's Y is chosen so the wire to the consumer pin it
    // feeds is level (siblings ordered by that Y, so wires don't cross). Any node nothing
    // claimed (an orphan pure node, or every node when the selection has no spine at all)
    // stacks in its own column to the left of everything else. FRectPacker slides a
    // candidate straight down on real collision against anything already placed, which is
    // what actually prevents overlap rather than trusting the grid arithmetic alone.
    FRectPacker Packer;
    for (const TPair<UEdGraphNode*, FVector2D>& Pair : OutPositions)
    {
        Packer.Reserve(Pair.Value.X, Pair.Value.Y, Pair.Value.X + EstimateNodeWidth(Pair.Key), Pair.Value.Y + EstimateNodeHeight(Pair.Key));
    }

    TMap<int32, TArray<UEdGraphNode*>> PureByDepth;
    TArray<UEdGraphNode*> Orphans;
    for (UEdGraphNode* Node : Nodes)
    {
        if (HasExecPin(Node)) continue;
        if (const FClaim* Claim = Claims.Find(Node)) PureByDepth.FindOrAdd(Claim->Depth).Add(Node);
        else Orphans.Add(Node);
    }

    // Shallow depths first: a node's consumer (owner or a shallower pure node) is already
    // placed by the time its own Y is derived from the consumer's pin.
    TArray<int32> PureDepths;
    PureByDepth.GetKeys(PureDepths);
    PureDepths.Sort();
    for (const int32 Depth : PureDepths)
    {
        TArray<TPair<float, UEdGraphNode*>> Group;
        for (UEdGraphNode* Node : PureByDepth[Depth])
        {
            const FClaim& Claim = Claims[Node];
            const float DesiredY = OutPositions[const_cast<UEdGraphNode*>(Claim.Consumer)].Y
                + PinCenterY(Claim.Consumer, Claim.ConsumerPin) - PinCenterY(Node, Claim.SourcePin);
            Group.Emplace(DesiredY, Node);
        }
        Algo::StableSort(Group, [](const TPair<float, UEdGraphNode*>& A, const TPair<float, UEdGraphNode*>& B)
        {
            return A.Key < B.Key || (A.Key == B.Key && A.Value->NodePosY < B.Value->NodePosY);
        });

        for (const TPair<float, UEdGraphNode*>& Entry : Group)
        {
            UEdGraphNode* Node = Entry.Value;
            const float Left = PureColumnLeft[Layer[Claims[Node].Owner]][Depth] + AnchorShiftX;
            // Never above the owner's exec row: the exec wire runs through the gap at the
            // owner's top, and a pure node on top of it would sit on that wire.
            const float MinY = OutPositions[Claims[Node].Owner].Y + ExecRowClearance;
            const float FinalY = Packer.Place(Left, Left + EstimateNodeWidth(Node), Entry.Key, EstimateNodeHeight(Node), MinY);
            OutPositions.Add(Node, FVector2D(Left, FinalY));
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
