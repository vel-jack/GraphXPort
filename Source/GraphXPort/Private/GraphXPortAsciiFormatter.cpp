#include "GraphXPortAsciiFormatter.h"

namespace
{
    FString SingleLine(FString Text)
    {
        Text.ReplaceInline(TEXT("\r"), TEXT(" "));
        Text.ReplaceInline(TEXT("\n"), TEXT(" "));
        return Text.TrimStartAndEnd();
    }

    bool IsExec(const FGraphXPortPin& Pin) { return Pin.Category == TEXT("exec"); }
    bool IsKnot(const FGraphXPortNode& Node) { return Node.ClassPath.Contains(TEXT("K2Node_Knot")); }
    bool IsEntry(const FGraphXPortNode& Node)
    {
        return Node.ClassPath.Contains(TEXT("FunctionEntry"))
            || Node.ClassPath.EndsWith(TEXT("K2Node_Event"))
            || Node.ClassPath.EndsWith(TEXT("K2Node_CustomEvent"));
    }
    bool HasExecution(const FGraphXPortNode& Node)
    {
        return Node.Pins.ContainsByPredicate([](const FGraphXPortPin& Pin) { return IsExec(Pin); });
    }

    FString Title(const FGraphXPortNode& Node)
    {
        FString Result = Node.MemberName.IsEmpty() ? Node.Title : Node.MemberName;
        if (!Node.MemberName.IsEmpty() && Node.ClassPath.Contains(TEXT("VariableSet")))
            Result = TEXT("Set ") + Node.MemberName;
        Result = SingleLine(Result);
        const int32 TargetSuffix = Result.Find(TEXT(" Target is "));
        if (TargetSuffix != INDEX_NONE) Result.LeftInline(TargetSuffix);
        return Result.IsEmpty() ? Node.Name : Result;
    }

    FString TypeName(const FGraphXPortPin& Pin)
    {
        FString Result = Pin.Category;
        if (!Pin.TypeObjectPath.IsEmpty())
        {
            int32 Dot = INDEX_NONE;
            int32 Slash = INDEX_NONE;
            Pin.TypeObjectPath.FindLastChar(TEXT('.'), Dot);
            Pin.TypeObjectPath.FindLastChar(TEXT('/'), Slash);
            Result = Pin.TypeObjectPath.Mid(FMath::Max(Dot, Slash) + 1);
            Result.RemoveFromEnd(TEXT("_C"));
        }
        if (!Pin.ContainerType.IsEmpty() && Pin.ContainerType != TEXT("none"))
            Result = Pin.ContainerType + TEXT("<") + Result + TEXT(">");
        return Result;
    }

    TArray<const FGraphXPortPin*> OrderedPins(const FGraphXPortNode& Node)
    {
        TArray<const FGraphXPortPin*> Result;
        for (const FGraphXPortPin& Pin : Node.Pins) Result.Add(&Pin);
        Result.Sort([](const FGraphXPortPin& A, const FGraphXPortPin& B)
        {
            const int32 AOrder = A.NodePinOrder == INDEX_NONE ? MAX_int32 : A.NodePinOrder;
            const int32 BOrder = B.NodePinOrder == INDEX_NONE ? MAX_int32 : B.NodePinOrder;
            return AOrder == BOrder ? A.Id < B.Id : AOrder < BOrder;
        });
        return Result;
    }

    FString ExecutionLabel(const FGraphXPortNode& Node, const FString& PinName)
    {
        // Only a Branch's "then" pin means TRUE. Switch case spelling is semantic.
        if (Node.ClassPath.Contains(TEXT("K2Node_IfThenElse")))
        {
            if (PinName.Equals(TEXT("then"), ESearchCase::IgnoreCase)) return TEXT("TRUE");
            if (PinName.Equals(TEXT("else"), ESearchCase::IgnoreCase)) return TEXT("FALSE");
        }
        if (Node.ClassPath.Contains(TEXT("K2Node_ExecutionSequence")))
        {
            FString Label = PinName;
            Label.ReplaceInline(TEXT("_"), TEXT(" "));
            return Label.ToUpper();
        }
        if (Node.ClassPath.Contains(TEXT("K2Node_Switch")))
            return PinName.Equals(TEXT("Default"), ESearchCase::IgnoreCase) ? TEXT("DEFAULT") : SingleLine(PinName);

        FString MacroIdentity = Node.MacroGraphPath + Node.Title;
        MacroIdentity.ReplaceInline(TEXT(" "), TEXT(""));
        const bool bLoop = MacroIdentity.Contains(TEXT("ForLoop")) || MacroIdentity.Contains(TEXT("ForEachLoop"))
            || MacroIdentity.Contains(TEXT("WhileLoop"));
        if (bLoop)
        {
            if (PinName.Equals(TEXT("LoopBody"), ESearchCase::IgnoreCase)
                || PinName.Equals(TEXT("Loop Body"), ESearchCase::IgnoreCase)) return TEXT("LOOP BODY");
            if (PinName.Equals(TEXT("Completed"), ESearchCase::IgnoreCase)) return TEXT("COMPLETED");
        }
        return PinName.IsEmpty() ? TEXT("NEXT") : SingleLine(PinName);
    }

    struct FEndpoint
    {
        FString NodeId;
        FString PinId;
        FString PinName;
        FString ExternalTitle;
        FString Note;
        bool bExternal = false;
    };

    struct FRoute
    {
        const FGraphXPortPin* Pin = nullptr;
        FEndpoint Endpoint;
    };

    class FTreeWriter
    {
    public:
        explicit FTreeWriter(const FGraphXPortDocument& InDocument) : Document(InDocument)
        {
            TSet<FString> AllIds;
            for (const FGraphXPortNode& Node : Document.Nodes)
            {
                Nodes.Add(&Node);
                ById.Add(Node.Id, &Node);
                AllIds.Add(Node.Id);
            }
            Nodes.Sort([](const FGraphXPortNode& A, const FGraphXPortNode& B) { return A.Id < B.Id; });
            for (const FGraphXPortEdge& Edge : Document.Edges) Edges.Add(&Edge);
            Edges.Sort([](const FGraphXPortEdge& A, const FGraphXPortEdge& B)
            {
                if (A.SourceNodeId != B.SourceNodeId) return A.SourceNodeId < B.SourceNodeId;
                if (A.SourcePinId != B.SourcePinId) return A.SourcePinId < B.SourcePinId;
                if (A.TargetNodeId != B.TargetNodeId) return A.TargetNodeId < B.TargetNodeId;
                return A.TargetPinId < B.TargetPinId;
            });
            for (const FGraphXPortExternalLink& Link : Document.ExternalLinks)
            {
                ExternalLinks.Add(&Link);
                AllIds.Add(Link.ExternalNodeId);
            }
            ExternalLinks.Sort([](const FGraphXPortExternalLink& A, const FGraphXPortExternalLink& B)
            {
                if (A.LocalNodeId != B.LocalNodeId) return A.LocalNodeId < B.LocalNodeId;
                if (A.LocalPinId != B.LocalPinId) return A.LocalPinId < B.LocalPinId;
                if (A.ExternalNodeId != B.ExternalNodeId) return A.ExternalNodeId < B.ExternalNodeId;
                return A.ExternalPinId < B.ExternalPinId;
            });
            for (const FString& Id : AllIds)
            {
                int32 Length = FMath::Min(8, Id.Len());
                while (Length < Id.Len())
                {
                    bool bCollision = false;
                    for (const FString& Other : AllIds)
                    {
                        if (Other != Id && Other.StartsWith(Id.Left(Length)))
                        {
                            bCollision = true;
                            break;
                        }
                    }
                    if (!bCollision) break;
                    Length = FMath::Min(Length + 4, Id.Len());
                }
                ShortIds.Add(Id, Id.Left(Length));
            }
        }

        FString Write()
        {
            const FGraphXPortNode* FunctionEntry = nullptr;
            int32 EntryCount = 0;
            for (const FGraphXPortNode* Node : Nodes)
            {
                if (IsEntry(*Node)) { ++EntryCount; FunctionEntry = Node; }
            }
            const bool bFunctionHeader = EntryCount == 1 && FunctionEntry->ClassPath.Contains(TEXT("FunctionEntry"));
            Output = Document.BlueprintName + TEXT(" :: ") + Document.GraphName;
            if (bFunctionHeader)
            {
                TArray<FString> Parameters;
                for (const FGraphXPortPin* Pin : OrderedPins(*FunctionEntry))
                    if (!IsExec(*Pin) && Pin->Direction == TEXT("output"))
                        Parameters.Add(Pin->Name + TEXT(": ") + TypeName(*Pin));
                Output = Document.BlueprintName + TEXT(" :: ") + Title(*FunctionEntry)
                    + TEXT("(") + FString::Join(Parameters, TEXT(", ")) + TEXT(") ") + Ref(FunctionEntry->Id);
            }
            Output += TEXT("\n");
            if (Nodes.IsEmpty()) return Output + TEXT("└─ [Empty]\n");

            // Events are independent roots. A function's sole entry is represented by its header.
            for (const FGraphXPortNode* Node : Nodes)
            {
                if (!IsEntry(*Node)) continue;
                if (bFunctionHeader && Node == FunctionEntry)
                {
                    Visited.Add(Node->Id);
                    Represented.Add(Node->Id);
                    const TArray<FRoute> Routes = RoutesFrom(*Node);
                    if (Routes.Num() == 1 && Routes[0].Endpoint.Note.IsEmpty()
                        && !Routes[0].Endpoint.bExternal && FindNode(Routes[0].Endpoint.NodeId))
                        RenderSequence(Routes[0].Endpoint, TEXT(""), TEXT(""), true, 0);
                    else RenderRoutes(Routes, *Node, TEXT(""), 0);
                }
                else RenderNodeRoot(*Node);
            }

            // Prefer natural roots, then cover rootless cycles and disconnected execution islands.
            TSet<FString> Incoming;
            for (const FGraphXPortNode* Node : Nodes)
                if (!IsKnot(*Node))
                    for (const FRoute& Route : RoutesFrom(*Node))
                        if (!Route.Endpoint.bExternal) Incoming.Add(Route.Endpoint.NodeId);
            for (const FGraphXPortNode* Node : Nodes)
                if (HasExecution(*Node) && !IsKnot(*Node) && !Incoming.Contains(Node->Id) && !Visited.Contains(Node->Id))
                    RenderNodeRoot(*Node);
            for (const FGraphXPortNode* Node : Nodes)
                if (HasExecution(*Node) && !IsKnot(*Node) && !Visited.Contains(Node->Id)) RenderNodeRoot(*Node);

            // Data-only selections have no execution root. Start with consumers, then cover cycles/isolated nodes.
            TSet<FString> DataSources;
            for (const FGraphXPortEdge* Edge : Edges)
                if (Edge->Kind == TEXT("data")) DataSources.Add(Edge->SourceNodeId);
            for (const FGraphXPortNode* Node : Nodes)
                if (!HasExecution(*Node) && !IsKnot(*Node) && !DataSources.Contains(Node->Id)
                    && !Represented.Contains(Node->Id)) RenderDataRoot(*Node);
            for (const FGraphXPortNode* Node : Nodes)
                if (!HasExecution(*Node) && !IsKnot(*Node) && !Represented.Contains(Node->Id)) RenderDataRoot(*Node);
            if (Represented.IsEmpty()) Output += TEXT("└─ [Reroute-only selection: no visible operation]\n");
            return Output;
        }

    private:
        const FGraphXPortDocument& Document;
        TArray<const FGraphXPortNode*> Nodes;
        TMap<FString, const FGraphXPortNode*> ById;
        TMap<FString, FString> ShortIds;
        TArray<const FGraphXPortEdge*> Edges;
        TArray<const FGraphXPortExternalLink*> ExternalLinks;
        TSet<FString> Visited;
        TSet<FString> Represented;
        FString Output;

        const FGraphXPortNode* FindNode(const FString& Id) const { return ById.FindRef(Id); }
        FString Ref(const FString& Id) const
        {
            const FString* Short = ShortIds.Find(Id);
            return TEXT("[") + (Short ? *Short : Id) + TEXT("]");
        }
        const FGraphXPortPin* FindPin(const FGraphXPortNode& Node, const FString& Id) const
        {
            return Node.Pins.FindByPredicate([&](const FGraphXPortPin& Pin) { return Pin.Id == Id; });
        }
        FString PinRef(const FGraphXPortNode& Node, const FString& PinId) const
        {
            const FGraphXPortPin* Pin = FindPin(Node, PinId);
            return Ref(Node.Id) + TEXT(".") + (Pin ? Pin->Name : PinId);
        }
        FEndpoint ExternalEndpoint(const FGraphXPortExternalLink& Link) const
        {
            FEndpoint Result;
            Result.NodeId = Link.ExternalNodeId;
            Result.PinId = Link.ExternalPinId;
            Result.PinName = Link.ExternalPinName;
            Result.ExternalTitle = SingleLine(Link.ExternalNodeTitle);
            Result.bExternal = true;
            return Result;
        }

        void ResolveEndpoint(const FEndpoint& Endpoint, TSet<FString> Seen, TArray<FEndpoint>& Results) const
        {
            const FGraphXPortNode* Node = FindNode(Endpoint.NodeId);
            if (Endpoint.bExternal || !Node || !IsKnot(*Node)) { Results.Add(Endpoint); return; }
            if (Seen.Contains(Node->Id))
            {
                FEndpoint Cycle = Endpoint;
                Cycle.Note = TEXT("Reroute cycle");
                Results.Add(Cycle);
                return;
            }
            Seen.Add(Node->Id);
            const int32 Before = Results.Num();
            for (const FGraphXPortEdge* Edge : Edges)
            {
                if (Edge->Kind != TEXT("execution") || Edge->SourceNodeId != Node->Id) continue;
                FEndpoint Next;
                Next.NodeId = Edge->TargetNodeId;
                Next.PinId = Edge->TargetPinId;
                Next.PinName = Edge->TargetPinName;
                ResolveEndpoint(Next, Seen, Results);
            }
            for (const FGraphXPortExternalLink* Link : ExternalLinks)
                if (Link->LocalNodeId == Node->Id && Link->Direction == TEXT("outgoing"))
                    Results.Add(ExternalEndpoint(*Link));
            if (Results.Num() == Before)
            {
                FEndpoint End;
                End.Note = TEXT("Unconnected reroute");
                Results.Add(End);
            }
        }

        TArray<FRoute> RoutesFrom(const FGraphXPortNode& Node) const
        {
            TArray<FRoute> Routes;
            for (const FGraphXPortPin* Pin : OrderedPins(Node))
            {
                if (!IsExec(*Pin) || Pin->Direction != TEXT("output")) continue;
                TArray<FEndpoint> Targets;
                for (const FGraphXPortEdge* Edge : Edges)
                {
                    if (Edge->Kind != TEXT("execution") || Edge->SourceNodeId != Node.Id || Edge->SourcePinId != Pin->Id) continue;
                    FEndpoint Next;
                    Next.NodeId = Edge->TargetNodeId;
                    Next.PinId = Edge->TargetPinId;
                    Next.PinName = Edge->TargetPinName;
                    ResolveEndpoint(Next, {}, Targets);
                }
                for (const FGraphXPortExternalLink* Link : ExternalLinks)
                    if (Link->LocalNodeId == Node.Id && Link->LocalPinId == Pin->Id && Link->Direction == TEXT("outgoing"))
                        Targets.Add(ExternalEndpoint(*Link));
                if (Targets.IsEmpty())
                {
                    FEndpoint End;
                    End.Note = TEXT("Unconnected");
                    Targets.Add(End);
                }
                for (const FEndpoint& Target : Targets)
                {
                    FRoute Route;
                    Route.Pin = Pin;
                    Route.Endpoint = Target;
                    Routes.Add(Route);
                }
            }
            return Routes;
        }

        FString InputValue(const FGraphXPortNode& Node, const FGraphXPortPin& Pin,
            TSet<FString> Seen, int32 Depth, bool& bConnected)
        {
            TArray<FString> Values;
            for (const FGraphXPortEdge* Edge : Edges)
                if (Edge->Kind == TEXT("data") && Edge->TargetNodeId == Node.Id && Edge->TargetPinId == Pin.Id)
                    Values.Add(Expression(Edge->SourceNodeId, Edge->SourcePinId, Seen, Depth + 1));
            for (const FGraphXPortExternalLink* Link : ExternalLinks)
                if (Link->LocalNodeId == Node.Id && Link->LocalPinId == Pin.Id && Link->Direction == TEXT("incoming"))
                    Values.Add(Ref(Link->ExternalNodeId) + TEXT(".") + Link->ExternalPinName + TEXT(" [External]"));
            bConnected = !Values.IsEmpty();
            if (bConnected) return FString::Join(Values, TEXT(" | "));
            if (!Pin.DefaultObjectPath.IsEmpty()) return SingleLine(Pin.DefaultObjectPath);
            return SingleLine(Pin.DefaultValue.IsEmpty() ? Pin.DefaultText : Pin.DefaultValue);
        }

        FString Expression(const FString& NodeId, const FString& PinId, TSet<FString> Seen, int32 Depth)
        {
            const FGraphXPortNode* Node = FindNode(NodeId);
            if (!Node) return Ref(NodeId) + TEXT(".") + PinId + TEXT(" [Missing]");
            Represented.Add(NodeId);
            const FString Key = NodeId + TEXT("|") + PinId;
            if (Seen.Contains(Key)) return PinRef(*Node, PinId) + TEXT(" [Data cycle]");
            if (Depth > 24) return PinRef(*Node, PinId) + TEXT(" [Depth limit]");
            Seen.Add(Key);
            if (IsKnot(*Node))
            {
                for (const FGraphXPortPin* Pin : OrderedPins(*Node))
                {
                    if (Pin->Direction != TEXT("input") || IsExec(*Pin)) continue;
                    bool bConnected = false;
                    const FString Value = InputValue(*Node, *Pin, Seen, Depth, bConnected);
                    if (!Value.IsEmpty()) return Value;
                }
                return TEXT("[Unconnected reroute]");
            }
            if (Node->ClassPath.Contains(TEXT("VariableSet")) && !Node->MemberName.IsEmpty())
                return Ref(NodeId) + TEXT(".") + Node->MemberName;
            if (Node->ClassPath.Contains(TEXT("CallFunction")) && !HasExecution(*Node))
            {
                TArray<FString> Arguments;
                for (const FGraphXPortPin* Pin : OrderedPins(*Node))
                {
                    if (Pin->Direction != TEXT("input") || IsExec(*Pin)) continue;
                    bool bConnected = false;
                    const FString Value = InputValue(*Node, *Pin, Seen, Depth, bConnected);
                    if (!bConnected && (Pin->bHidden || Pin->Name.Equals(TEXT("self"), ESearchCase::IgnoreCase))) continue;
                    Arguments.Add(Pin->Name + TEXT("=") + (Value.IsEmpty() ? TEXT("[Unset]") : Value));
                }
                const FGraphXPortPin* ResultPin = FindPin(*Node, PinId);
                return Ref(NodeId) + TEXT(" ") + Title(*Node) + TEXT("(") + FString::Join(Arguments, TEXT(", "))
                    + TEXT(").") + (ResultPin ? ResultPin->Name : PinId);
            }
            return PinRef(*Node, PinId);
        }

        TArray<FString> Inputs(const FGraphXPortNode& Node)
        {
            TArray<FString> Lines;
            for (const FGraphXPortPin* Pin : OrderedPins(Node))
            {
                if (IsExec(*Pin) || Pin->Direction != TEXT("input")) continue;
                bool bConnected = false;
                const FString Value = InputValue(Node, *Pin, {}, 0, bConnected);
                if (!bConnected && (Pin->bHidden || Pin->Name.Equals(TEXT("self"), ESearchCase::IgnoreCase))) continue;
                if (!Value.IsEmpty()) Lines.Add(Pin->Name + (bConnected ? TEXT(" ← ") : TEXT(" = ")) + Value);
            }
            return Lines;
        }

        void RenderInputs(const TArray<FString>& Lines, const FString& Prefix, bool bMoreChildren)
        {
            for (int32 Index = 0; Index < Lines.Num(); ++Index)
                Output += Prefix + (bMoreChildren || Index + 1 < Lines.Num() ? TEXT("├─ ") : TEXT("└─ ")) + Lines[Index] + TEXT("\n");
        }

        FString IncomingSuffix(const FEndpoint& Endpoint) const
        {
            const FString& Pin = Endpoint.PinName;
            if (Pin.IsEmpty() || Pin.Equals(TEXT("execute"), ESearchCase::IgnoreCase)
                || Pin.Equals(TEXT("exec"), ESearchCase::IgnoreCase) || Pin.Equals(TEXT("in"), ESearchCase::IgnoreCase))
                return FString();
            return TEXT(" [in: ") + Pin + TEXT("]");
        }

        void RenderRoutes(const TArray<FRoute>& Routes, const FGraphXPortNode& Node, const FString& Prefix, int32 Depth)
        {
            if (Routes.IsEmpty()) { Output += Prefix + TEXT("└─ [End]\n"); return; }
            for (int32 Index = 0; Index < Routes.Num(); ++Index)
            {
                const FRoute& Route = Routes[Index];
                RenderSequence(Route.Endpoint, Prefix, ExecutionLabel(Node, Route.Pin->Name), Index + 1 == Routes.Num(), Depth + 1);
            }
        }

        void RenderSequence(FEndpoint Endpoint, FString Prefix, FString Label, bool bLast, int32 Depth)
        {
            bool bLabelled = !Label.IsEmpty();
            while (true)
            {
                const FGraphXPortNode* Node = FindNode(Endpoint.NodeId);
                const FString Lead = Label.IsEmpty() ? FString() : Label + TEXT(" → ");
                const FString Connector = bLast ? TEXT("└─ ") : TEXT("├─ ");
                if (!Endpoint.Note.IsEmpty())
                {
                    Output += Prefix + Connector + Lead + TEXT("[") + Endpoint.Note + TEXT("]\n");
                    return;
                }
                if (Endpoint.bExternal || !Node)
                {
                    Output += Prefix + Connector + Lead + Ref(Endpoint.NodeId) + TEXT(" ") + Endpoint.ExternalTitle
                        + IncomingSuffix(Endpoint) + (Endpoint.bExternal ? TEXT(" [External]\n") : TEXT(" [Missing]\n"));
                    return;
                }
                const FString NodeText = Ref(Node->Id) + TEXT(" ") + Title(*Node) + IncomingSuffix(Endpoint);
                if (Visited.Contains(Node->Id) || Depth > 128)
                {
                    Output += Prefix + Connector + Lead + TEXT("↩ ") + NodeText
                        + (Depth > 128 ? TEXT(" [Depth limit]\n") : TEXT("\n"));
                    return;
                }
                Visited.Add(Node->Id);
                Represented.Add(Node->Id);
                const TArray<FRoute> Routes = RoutesFrom(*Node);
                const bool bRoutineNode = !Node->ClassPath.Contains(TEXT("K2Node_IfThenElse"))
                    && !Node->ClassPath.Contains(TEXT("K2Node_ExecutionSequence"))
                    && !Node->ClassPath.Contains(TEXT("K2Node_Switch"));
                const bool bLinear = bRoutineNode && Routes.Num() == 1
                    && Routes[0].Pin->Name.Equals(TEXT("then"), ESearchCase::IgnoreCase)
                    && Routes[0].Endpoint.Note.IsEmpty() && !Routes[0].Endpoint.bExternal;
                const bool bTerminal = Routes.IsEmpty() || (bRoutineNode && Routes.Num() == 1
                    && Routes[0].Pin->Name.Equals(TEXT("then"), ESearchCase::IgnoreCase)
                    && Routes[0].Endpoint.Note == TEXT("Unconnected"));
                const bool bNodeLast = bLabelled ? bLast : !(bLinear || bTerminal);
                Output += Prefix + (bNodeLast ? TEXT("└─ ") : TEXT("├─ ")) + Lead + NodeText + TEXT("\n");
                const FString ChildPrefix = Prefix + (bNodeLast ? TEXT("   ") : TEXT("│  "));
                const TArray<FString> InputLines = Inputs(*Node);
                RenderInputs(InputLines, ChildPrefix, bLabelled || (!bLinear && !bTerminal));
                if (bTerminal)
                {
                    Output += (bLabelled ? ChildPrefix : Prefix) + TEXT("└─ [End]\n");
                    return;
                }
                if (bLinear)
                {
                    Endpoint = Routes[0].Endpoint;
                    if (bLabelled) Prefix = ChildPrefix;
                    Label.Reset();
                    bLabelled = false;
                    bLast = true;
                    continue;
                }
                RenderRoutes(Routes, *Node, ChildPrefix, Depth);
                return;
            }
        }

        void RenderNodeRoot(const FGraphXPortNode& Node)
        {
            FEndpoint Start;
            Start.NodeId = Node.Id;
            RenderSequence(Start, TEXT(""), TEXT(""), true, 0);
        }
        void RenderDataRoot(const FGraphXPortNode& Node)
        {
            Represented.Add(Node.Id);
            Output += TEXT("└─ ") + Ref(Node.Id) + TEXT(" ") + Title(Node) + TEXT(" [Data]\n");
            RenderInputs(Inputs(Node), TEXT("   "), false);
        }
    };
}

FString FGraphXPortAsciiFormatter::Format(const FGraphXPortDocument& Document)
{
    return FTreeWriter(Document).Write();
}

