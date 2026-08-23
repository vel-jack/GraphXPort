#include "GraphXPortFormatter.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    TSharedRef<FJsonObject> PinJson(const FGraphXPortPin& Pin)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("nodePinOrder"), Pin.NodePinOrder);
        Json->SetStringField(TEXT("id"), Pin.Id);
        Json->SetStringField(TEXT("name"), Pin.Name);
        Json->SetStringField(TEXT("direction"), Pin.Direction);
        Json->SetStringField(TEXT("category"), Pin.Category);
        Json->SetStringField(TEXT("subCategory"), Pin.SubCategory);
        Json->SetStringField(TEXT("containerType"), Pin.ContainerType);
        Json->SetStringField(TEXT("typeObjectPath"), Pin.TypeObjectPath);
        Json->SetStringField(TEXT("defaultValue"), Pin.DefaultValue);
        Json->SetStringField(TEXT("defaultObjectPath"), Pin.DefaultObjectPath);
        Json->SetStringField(TEXT("defaultText"), Pin.DefaultText);
        Json->SetBoolField(TEXT("hidden"), Pin.bHidden);
        Json->SetBoolField(TEXT("advancedView"), Pin.bAdvancedView);
        Json->SetBoolField(TEXT("orphaned"), Pin.bOrphaned);
        return Json;
    }

    FString OneLine(FString Value)
    {
        Value.ReplaceInline(TEXT("\r"), TEXT(" "));
        Value.ReplaceInline(TEXT("\n"), TEXT(" "));
        return Value;
    }

    FString ShortId(const FString& Id)
    {
        return Id.Left(FMath::Min(8, Id.Len()));
    }

    FString MermaidEscape(FString Value)
    {
        Value = OneLine(MoveTemp(Value));
        Value.ReplaceInline(TEXT("\""), TEXT("'"));
        Value.ReplaceInline(TEXT("["), TEXT("("));
        Value.ReplaceInline(TEXT("]"), TEXT(")"));
        return Value;
    }

    bool IsExecPin(const FGraphXPortPin& Pin)
    {
        return Pin.Category == TEXT("exec");
    }

    bool IsRerouteNode(const FGraphXPortNode& Node)
    {
        return Node.ClassPath.Contains(TEXT("K2Node_Knot"));
    }

    FString CompactNodeTitle(const FGraphXPortNode& Node)
    {
        FString Result;
        if (!Node.MemberName.IsEmpty() && Node.ClassPath.Contains(TEXT("VariableSet")))
        {
            Result = TEXT("Set ") + Node.MemberName;
        }
        else
        {
            Result = !Node.MemberName.IsEmpty() ? Node.MemberName : Node.Title;
        }
        Result = OneLine(MoveTemp(Result));
        Result.TrimStartAndEndInline();
        const int32 TargetSuffix = Result.Find(TEXT(" Target is "));
        if (TargetSuffix != INDEX_NONE)
        {
            Result.LeftInline(TargetSuffix);
        }
        return Result.IsEmpty() ? Node.Name : Result;
    }

    FString CompactType(const FGraphXPortPin& Pin)
    {
        FString Result = Pin.TypeObjectPath;
        if (!Result.IsEmpty())
        {
            int32 SlashIndex = INDEX_NONE;
            int32 DotIndex = INDEX_NONE;
            Result.FindLastChar(TEXT('/'), SlashIndex);
            Result.FindLastChar(TEXT('.'), DotIndex);
            Result.RightChopInline(FMath::Max(SlashIndex, DotIndex) + 1);
            Result.RemoveFromStart(TEXT("SKEL_"));
            Result.RemoveFromStart(TEXT("REINST_"));
            Result.RemoveFromEnd(TEXT("_C"));
        }
        else
        {
            Result = Pin.Category;
        }
        if (Pin.ContainerType != TEXT("none") && !Pin.ContainerType.IsEmpty())
        {
            Result = Pin.ContainerType + TEXT("<") + Result + TEXT(">");
        }
        return Result;
    }

    FString BranchLabel(FString PinName)
    {
        if (PinName.Equals(TEXT("then"), ESearchCase::IgnoreCase)
            || PinName.Equals(TEXT("true"), ESearchCase::IgnoreCase))
        {
            return TEXT("TRUE");
        }
        if (PinName.Equals(TEXT("else"), ESearchCase::IgnoreCase)
            || PinName.Equals(TEXT("false"), ESearchCase::IgnoreCase))
        {
            return TEXT("FALSE");
        }
        PinName.TrimStartAndEndInline();
        PinName.ReplaceInline(TEXT("_"), TEXT(" "));
        return PinName.IsEmpty() ? TEXT("NEXT") : PinName.ToUpper();
    }
}

FString FGraphXPortFormatter::Format(const FGraphXPortDocument& Document, const EGraphXPortFormat FormatType)
{
    switch (FormatType)
    {
    case EGraphXPortFormat::AiText: return ToAiText(Document);
    case EGraphXPortFormat::Ascii: return ToAscii(Document);
    case EGraphXPortFormat::Mermaid: return ToMermaid(Document);
    default: return ToJson(Document);
    }
}

FString FGraphXPortFormatter::Extension(const EGraphXPortFormat FormatType)
{
    switch (FormatType)
    {
    case EGraphXPortFormat::AiText: return TEXT("graphx.md");
    case EGraphXPortFormat::Ascii: return TEXT("graphx.txt");
    case EGraphXPortFormat::Mermaid: return TEXT("graphx.mmd");
    default: return TEXT("graphx.json");
    }
}

FString FGraphXPortFormatter::FileFilter(const EGraphXPortFormat FormatType)
{
    switch (FormatType)
    {
    case EGraphXPortFormat::AiText: return TEXT("GraphXPort AI text (*.graphx.md)|*.graphx.md");
    case EGraphXPortFormat::Ascii: return TEXT("GraphXPort ASCII (*.graphx.txt)|*.graphx.txt");
    case EGraphXPortFormat::Mermaid: return TEXT("GraphXPort Mermaid (*.graphx.mmd)|*.graphx.mmd");
    default: return TEXT("GraphXPort JSON (*.graphx.json)|*.graphx.json");
    }
}

FString FGraphXPortFormatter::ToJson(const FGraphXPortDocument& Document)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("formatVersion"), Document.FormatVersion);
    Root->SetStringField(TEXT("generator"), TEXT("GraphXPort"));
    Root->SetStringField(TEXT("blueprintPath"), Document.BlueprintPath);
    Root->SetStringField(TEXT("blueprintName"), Document.BlueprintName);
    Root->SetStringField(TEXT("graphName"), Document.GraphName);
    Root->SetStringField(TEXT("graphId"), Document.GraphId);
    Root->SetStringField(TEXT("scope"), Document.Scope);

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const FGraphXPortNode& Node : Document.Nodes)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("id"), Node.Id);
        Json->SetStringField(TEXT("name"), Node.Name);
        Json->SetStringField(TEXT("classPath"), Node.ClassPath);
        Json->SetStringField(TEXT("title"), Node.Title);
        Json->SetStringField(TEXT("comment"), Node.Comment);
        Json->SetStringField(TEXT("memberName"), Node.MemberName);
        Json->SetStringField(TEXT("memberParentPath"), Node.MemberParentPath);
        Json->SetStringField(TEXT("macroGraphPath"), Node.MacroGraphPath);
        TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Node.PositionX);
        Position->SetNumberField(TEXT("y"), Node.PositionY);
        Json->SetObjectField(TEXT("position"), Position);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const FGraphXPortPin& Pin : Node.Pins)
        {
            Pins.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
        }
        Json->SetArrayField(TEXT("pins"), Pins);
        Nodes.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("nodes"), Nodes);

    TArray<TSharedPtr<FJsonValue>> Edges;
    for (const FGraphXPortEdge& Edge : Document.Edges)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("sourceNodeId"), Edge.SourceNodeId);
        Json->SetStringField(TEXT("sourcePinId"), Edge.SourcePinId);
        Json->SetStringField(TEXT("sourcePinName"), Edge.SourcePinName);
        Json->SetStringField(TEXT("targetNodeId"), Edge.TargetNodeId);
        Json->SetStringField(TEXT("targetPinId"), Edge.TargetPinId);
        Json->SetStringField(TEXT("targetPinName"), Edge.TargetPinName);
        Json->SetStringField(TEXT("kind"), Edge.Kind);
        Edges.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("edges"), Edges);

    TArray<TSharedPtr<FJsonValue>> ExternalLinks;
    for (const FGraphXPortExternalLink& Link : Document.ExternalLinks)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("localNodeId"), Link.LocalNodeId);
        Json->SetStringField(TEXT("localPinId"), Link.LocalPinId);
        Json->SetStringField(TEXT("localPinName"), Link.LocalPinName);
        Json->SetStringField(TEXT("direction"), Link.Direction);
        Json->SetStringField(TEXT("externalNodeId"), Link.ExternalNodeId);
        Json->SetStringField(TEXT("externalNodeTitle"), Link.ExternalNodeTitle);
        Json->SetStringField(TEXT("externalNodeClassPath"), Link.ExternalNodeClassPath);
        Json->SetStringField(TEXT("externalPinId"), Link.ExternalPinId);
        Json->SetStringField(TEXT("externalPinName"), Link.ExternalPinName);
        ExternalLinks.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("externalLinks"), ExternalLinks);

    FString Output;
    const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
    FJsonSerializer::Serialize(Root, Writer);
    return Output;
}

FString FGraphXPortFormatter::ToAiText(const FGraphXPortDocument& Document)
{
    FString Output = FString::Printf(
        TEXT("# Blueprint Graph: %s\n\n- Blueprint: `%s`\n- Scope: `%s`\n- Nodes: %d\n- Connections: %d\n- External links: %d\n\n## Nodes\n"),
        *Document.GraphName, *Document.BlueprintPath, *Document.Scope,
        Document.Nodes.Num(), Document.Edges.Num(), Document.ExternalLinks.Num());

    for (const FGraphXPortNode& Node : Document.Nodes)
    {
        Output += FString::Printf(TEXT("\n- `%s` **%s**"), *ShortId(Node.Id), *OneLine(Node.Title));
        if (!Node.MemberName.IsEmpty()) Output += FString::Printf(TEXT(" -> `%s`"), *Node.MemberName);
        if (!Node.MemberParentPath.IsEmpty()) Output += FString::Printf(TEXT(" on `%s`"), *Node.MemberParentPath);
        TArray<FString> Defaults;
        for (const FGraphXPortPin& Pin : Node.Pins)
        {
            const FString Value = !Pin.DefaultObjectPath.IsEmpty() ? Pin.DefaultObjectPath
                : (!Pin.DefaultValue.IsEmpty() ? Pin.DefaultValue : Pin.DefaultText);
            if (Pin.Direction == TEXT("input") && !Value.IsEmpty())
            {
                Defaults.Add(Pin.Name + TEXT("=") + OneLine(Value));
            }
        }
        if (!Defaults.IsEmpty()) Output += TEXT(" | ") + FString::Join(Defaults, TEXT(", "));
    }

    Output += TEXT("\n\n## Connections\n");
    for (const FGraphXPortEdge& Edge : Document.Edges)
    {
        Output += FString::Printf(TEXT("- `%s.%s` -> `%s.%s` (%s)\n"),
            *ShortId(Edge.SourceNodeId), *Edge.SourcePinName,
            *ShortId(Edge.TargetNodeId), *Edge.TargetPinName, *Edge.Kind);
    }
    if (!Document.ExternalLinks.IsEmpty())
    {
        Output += TEXT("\n## External Connections\n");
        for (const FGraphXPortExternalLink& Link : Document.ExternalLinks)
        {
            Output += FString::Printf(TEXT("- `%s.%s` %s `%s.%s` (%s)\n"),
                *ShortId(Link.LocalNodeId), *Link.LocalPinName, *Link.Direction,
                *ShortId(Link.ExternalNodeId), *Link.ExternalPinName, *OneLine(Link.ExternalNodeTitle));
        }
    }
    return Output;
}

FString FGraphXPortFormatter::ToAscii(const FGraphXPortDocument& Document)
{
    TMap<FString, const FGraphXPortNode*> NodesById;
    TMap<FString, FString> DisplayIds;
    for (const FGraphXPortNode& Node : Document.Nodes)
    {
        NodesById.Add(Node.Id, &Node);

        int32 PrefixLength = FMath::Min(8, Node.Id.Len());
        while (PrefixLength < Node.Id.Len())
        {
            const FString Candidate = Node.Id.Left(PrefixLength);
            const bool bCollision = Document.Nodes.ContainsByPredicate([&](const FGraphXPortNode& Other)
            {
                return Other.Id != Node.Id && Other.Id.StartsWith(Candidate);
            });
            if (!bCollision) break;
            PrefixLength = FMath::Min(PrefixLength + 4, Node.Id.Len());
        }
        DisplayIds.Add(Node.Id, Node.Id.Left(PrefixLength));
    }

    const auto NodeRef = [&DisplayIds](const FString& NodeId)
    {
        const FString* DisplayId = DisplayIds.Find(NodeId);
        return TEXT("[") + (DisplayId != nullptr ? *DisplayId : ShortId(NodeId)) + TEXT("]");
    };

    const auto FindPin = [](const FGraphXPortNode& Node, const FString& PinId) -> const FGraphXPortPin*
    {
        return Node.Pins.FindByPredicate([&PinId](const FGraphXPortPin& Pin) { return Pin.Id == PinId; });
    };

    const auto IncomingDataEdge = [&Document](const FString& NodeId, const FString& PinId) -> const FGraphXPortEdge*
    {
        return Document.Edges.FindByPredicate([&](const FGraphXPortEdge& Edge)
        {
            return Edge.Kind == TEXT("data") && Edge.TargetNodeId == NodeId && Edge.TargetPinId == PinId;
        });
    };

    const auto IncomingExternalLink = [&Document](const FString& NodeId, const FString& PinId) -> const FGraphXPortExternalLink*
    {
        return Document.ExternalLinks.FindByPredicate([&](const FGraphXPortExternalLink& Link)
        {
            return Link.Direction == TEXT("incoming") && Link.LocalNodeId == NodeId && Link.LocalPinId == PinId;
        });
    };

    TFunction<FString(const FString&, const FString&, TSet<FString>&)> DataExpression;
    DataExpression = [&](const FString& NodeId, const FString& PinId, TSet<FString>& Visiting) -> FString
    {
        const FGraphXPortNode* const* NodePtr = NodesById.Find(NodeId);
        if (NodePtr == nullptr || *NodePtr == nullptr)
        {
            return TEXT("?");
        }
        const FGraphXPortNode& Node = **NodePtr;
        const FString VisitKey = NodeId + TEXT("|") + PinId;
        if (Visiting.Contains(VisitKey))
        {
            return NodeRef(Node.Id) + TEXT(" ") + CompactNodeTitle(Node);
        }
        Visiting.Add(VisitKey);

        if (IsRerouteNode(Node))
        {
            for (const FGraphXPortPin& Pin : Node.Pins)
            {
                if (Pin.Direction == TEXT("input") && !IsExecPin(Pin))
                {
                    if (const FGraphXPortEdge* Edge = IncomingDataEdge(Node.Id, Pin.Id))
                    {
                        return DataExpression(Edge->SourceNodeId, Edge->SourcePinId, Visiting);
                    }
                }
            }
        }

        if (Node.ClassPath.Contains(TEXT("FunctionEntry")))
        {
            if (const FGraphXPortPin* Pin = FindPin(Node, PinId))
            {
                return Pin->Name;
            }
        }

        const FString Label = CompactNodeTitle(Node);
        if (Node.ClassPath.Contains(TEXT("VariableGet")))
        {
            return NodeRef(Node.Id) + TEXT(".") + Label;
        }
        if (Node.ClassPath.Contains(TEXT("VariableSet")))
        {
            return NodeRef(Node.Id) + TEXT(".") + Node.MemberName;
        }

        if (Node.ClassPath.Contains(TEXT("CallFunction")))
        {
            TArray<FString> Arguments;
            for (const FGraphXPortPin& Pin : Node.Pins)
            {
                if (Pin.Direction != TEXT("input") || IsExecPin(Pin) || Pin.Name.Equals(TEXT("self"), ESearchCase::IgnoreCase))
                {
                    continue;
                }
                if (const FGraphXPortEdge* Edge = IncomingDataEdge(Node.Id, Pin.Id))
                {
                    TSet<FString> NestedVisiting = Visiting;
                    Arguments.Add(DataExpression(Edge->SourceNodeId, Edge->SourcePinId, NestedVisiting));
                }
                else
                {
                    const FString Value = !Pin.DefaultObjectPath.IsEmpty() ? Pin.DefaultObjectPath
                        : (!Pin.DefaultValue.IsEmpty() ? Pin.DefaultValue : Pin.DefaultText);
                    if (!Value.IsEmpty()) Arguments.Add(OneLine(Value));
                }
            }
            return NodeRef(Node.Id) + TEXT(" ") + Label + TEXT("(") + FString::Join(Arguments, TEXT(", ")) + TEXT(")");
        }
        if (const FGraphXPortPin* Pin = FindPin(Node, PinId))
        {
            return NodeRef(Node.Id) + TEXT(".") + Pin->Name;
        }
        return NodeRef(Node.Id) + TEXT(" ") + Label;
    };

    const auto ResolveExecTarget = [&](const FGraphXPortEdge& InitialEdge) -> const FGraphXPortNode*
    {
        const FGraphXPortEdge* Edge = &InitialEdge;
        TSet<FString> Seen;
        while (Edge != nullptr)
        {
            const FGraphXPortNode* const* NodePtr = NodesById.Find(Edge->TargetNodeId);
            if (NodePtr == nullptr || *NodePtr == nullptr) return nullptr;
            const FGraphXPortNode* Node = *NodePtr;
            if (!IsRerouteNode(*Node)) return Node;
            if (Seen.Contains(Node->Id)) return nullptr;
            Seen.Add(Node->Id);
            Edge = Document.Edges.FindByPredicate([&](const FGraphXPortEdge& Candidate)
            {
                return Candidate.Kind == TEXT("execution") && Candidate.SourceNodeId == Node->Id;
            });
        }
        return nullptr;
    };

    struct FAsciiExecLink
    {
        FString PinId;
        FString PinName;
        int32 PinOrder = MAX_int32;
        const FGraphXPortNode* Target = nullptr;
        const FGraphXPortExternalLink* External = nullptr;
    };

    const auto ExecLinksFrom = [&](const FGraphXPortNode& SourceNode)
    {
        TArray<FAsciiExecLink> Result;
        for (const FGraphXPortEdge& Edge : Document.Edges)
        {
            if (Edge.Kind == TEXT("execution") && Edge.SourceNodeId == SourceNode.Id)
            {
                FAsciiExecLink& Link = Result.AddDefaulted_GetRef();
                Link.PinId = Edge.SourcePinId;
                Link.PinName = Edge.SourcePinName;
                Link.Target = ResolveExecTarget(Edge);
                if (const FGraphXPortPin* Pin = FindPin(SourceNode, Edge.SourcePinId)) Link.PinOrder = Pin->NodePinOrder;
            }
        }
        for (const FGraphXPortExternalLink& External : Document.ExternalLinks)
        {
            if (External.Direction != TEXT("outgoing") || External.LocalNodeId != SourceNode.Id) continue;
            const FGraphXPortPin* Pin = FindPin(SourceNode, External.LocalPinId);
            if (Pin == nullptr || !IsExecPin(*Pin)) continue;
            FAsciiExecLink& Link = Result.AddDefaulted_GetRef();
            Link.PinId = External.LocalPinId;
            Link.PinName = External.LocalPinName;
            Link.PinOrder = Pin->NodePinOrder;
            Link.External = &External;
        }
        Result.Sort([](const FAsciiExecLink& A, const FAsciiExecLink& B)
        {
            if (A.PinOrder != B.PinOrder) return A.PinOrder < B.PinOrder;
            if (A.PinName != B.PinName) return A.PinName < B.PinName;
            const FString ATarget = A.Target != nullptr ? A.Target->Id : (A.External != nullptr ? A.External->ExternalNodeId : FString());
            const FString BTarget = B.Target != nullptr ? B.Target->Id : (B.External != nullptr ? B.External->ExternalNodeId : FString());
            return ATarget < BTarget;
        });
        return Result;
    };

    const FGraphXPortNode* EntryNode = Document.Nodes.FindByPredicate([](const FGraphXPortNode& Node)
    {
        return Node.ClassPath.Contains(TEXT("FunctionEntry")) || Node.ClassPath.Contains(TEXT("Event"));
    });

    FString Header = Document.GraphName;
    if (EntryNode != nullptr)
    {
        Header = Document.BlueprintName + TEXT(" :: ") + CompactNodeTitle(*EntryNode);
        TArray<const FGraphXPortPin*> ParameterPins;
        for (const FGraphXPortPin& Pin : EntryNode->Pins)
        {
            if (Pin.Direction == TEXT("output") && !IsExecPin(Pin)) ParameterPins.Add(&Pin);
        }
        ParameterPins.Sort([](const FGraphXPortPin& A, const FGraphXPortPin& B)
        {
            return A.NodePinOrder < B.NodePinOrder;
        });
        TArray<FString> Parameters;
        for (const FGraphXPortPin* Pin : ParameterPins)
        {
            Parameters.Add(Pin->Name + TEXT(": ") + CompactType(*Pin));
        }
        if (!Parameters.IsEmpty()) Header += TEXT("(") + FString::Join(Parameters, TEXT(", ")) + TEXT(")");
    }
    FString Output = Header + TEXT("\n");

    TSet<FString> VisitedExecNodes;
    TFunction<void(const FGraphXPortNode*, FString, FString, bool)> RenderSequence;
    RenderSequence = [&](const FGraphXPortNode* StartNode, FString Prefix, FString EdgeLabel, bool bFirstLineLast)
    {
        const FGraphXPortNode* Node = StartNode;
        while (Node != nullptr)
        {
            if (VisitedExecNodes.Contains(Node->Id))
            {
                Output += Prefix + (bFirstLineLast ? TEXT("└─ ") : TEXT("├─ "));
                if (!EdgeLabel.IsEmpty()) Output += EdgeLabel + TEXT(" → ");
                Output += TEXT("↩ ") + NodeRef(Node->Id) + TEXT(" ") + CompactNodeTitle(*Node) + TEXT("\n");
                return;
            }
            VisitedExecNodes.Add(Node->Id);

            Output += Prefix + (bFirstLineLast ? TEXT("└─ ") : TEXT("├─ "));
            if (!EdgeLabel.IsEmpty()) Output += EdgeLabel + TEXT(" → ");
            Output += NodeRef(Node->Id) + TEXT(" ") + CompactNodeTitle(*Node) + TEXT("\n");

            const FString DetailPrefix = Prefix + (bFirstLineLast ? TEXT("   ") : TEXT("│  "));

            TArray<FString> Inputs;
            TArray<const FGraphXPortPin*> InputPins;
            for (const FGraphXPortPin& Pin : Node->Pins)
            {
                if (Pin.Direction == TEXT("input") && !IsExecPin(Pin) && !Pin.bHidden
                    && !Pin.Name.Equals(TEXT("self"), ESearchCase::IgnoreCase)) InputPins.Add(&Pin);
            }
            InputPins.Sort([](const FGraphXPortPin& A, const FGraphXPortPin& B)
            {
                return A.NodePinOrder < B.NodePinOrder;
            });
            for (const FGraphXPortPin* Pin : InputPins)
            {
                FString Value;
                FString Operator = TEXT(" = ");
                if (const FGraphXPortEdge* DataEdge = IncomingDataEdge(Node->Id, Pin->Id))
                {
                    TSet<FString> Visiting;
                    Value = DataExpression(DataEdge->SourceNodeId, DataEdge->SourcePinId, Visiting);
                    Operator = TEXT(" ← ");
                }
                else if (const FGraphXPortExternalLink* External = IncomingExternalLink(Node->Id, Pin->Id))
                {
                    Value = TEXT("[") + ShortId(External->ExternalNodeId) + TEXT("] ")
                        + OneLine(External->ExternalNodeTitle) + TEXT(".") + External->ExternalPinName + TEXT(" [External]");
                    Operator = TEXT(" ← ");
                }
                else
                {
                    Value = !Pin->DefaultObjectPath.IsEmpty() ? Pin->DefaultObjectPath
                        : (!Pin->DefaultValue.IsEmpty() ? Pin->DefaultValue : Pin->DefaultText);
                }
                if (!Value.IsEmpty()) Inputs.Add(Pin->Name + Operator + OneLine(Value));
            }
            for (int32 Index = 0; Index < Inputs.Num(); ++Index)
            {
                Output += DetailPrefix + (Index + 1 == Inputs.Num() ? TEXT("└─ ") : TEXT("├─ ")) + Inputs[Index] + TEXT("\n");
            }

            const TArray<FAsciiExecLink> NextLinks = ExecLinksFrom(*Node);
            if (NextLinks.IsEmpty())
            {
                Output += Prefix + TEXT("└─ [End]\n");
                return;
            }
            if (NextLinks.Num() == 1 && NextLinks[0].Target != nullptr
                && (NextLinks[0].PinName.IsEmpty() || NextLinks[0].PinName.Equals(TEXT("then"), ESearchCase::IgnoreCase)))
            {
                Node = NextLinks[0].Target;
                if (!EdgeLabel.IsEmpty()) Prefix = DetailPrefix;
                EdgeLabel.Reset();
                bFirstLineLast = false;
                continue;
            }

            const FString ChildrenPrefix = DetailPrefix;
            for (int32 Index = 0; Index < NextLinks.Num(); ++Index)
            {
                const FAsciiExecLink& NextLink = NextLinks[Index];
                const bool bLast = Index + 1 == NextLinks.Num();
                const FString Label = BranchLabel(NextLink.PinName);
                if (NextLink.Target != nullptr)
                {
                    RenderSequence(NextLink.Target, ChildrenPrefix, Label, bLast);
                }
                else if (NextLink.External != nullptr)
                {
                    Output += ChildrenPrefix + (bLast ? TEXT("└─ ") : TEXT("├─ ")) + Label + TEXT(" → ")
                        + TEXT("[") + ShortId(NextLink.External->ExternalNodeId) + TEXT("] ")
                        + OneLine(NextLink.External->ExternalNodeTitle) + TEXT(" [External]\n");
                }
                else
                {
                    Output += ChildrenPrefix + (bLast ? TEXT("└─ ") : TEXT("├─ "))
                        + Label + TEXT(" → [External]\n");
                }
            }
            return;
        }
    };

    if (EntryNode != nullptr)
    {
        const TArray<FAsciiExecLink> EntryLinks = ExecLinksFrom(*EntryNode);
        for (int32 Index = 0; Index < EntryLinks.Num(); ++Index)
        {
            const FAsciiExecLink& Link = EntryLinks[Index];
            if (Link.Target != nullptr) RenderSequence(Link.Target, FString(), FString(), false);
        }
    }
    else
    {
        TSet<FString> HasIncomingExecution;
        for (const FGraphXPortEdge& Edge : Document.Edges)
        {
            if (Edge.Kind == TEXT("execution")) HasIncomingExecution.Add(Edge.TargetNodeId);
        }
        TArray<const FGraphXPortNode*> Roots;
        for (const FGraphXPortNode& Node : Document.Nodes)
        {
            const bool bHasExecPin = Node.Pins.ContainsByPredicate([](const FGraphXPortPin& Pin) { return IsExecPin(Pin); });
            if (bHasExecPin && !IsRerouteNode(Node) && !HasIncomingExecution.Contains(Node.Id)) Roots.Add(&Node);
        }
        Roots.Sort([](const FGraphXPortNode& A, const FGraphXPortNode& B) { return A.Id < B.Id; });
        for (int32 Index = 0; Index < Roots.Num(); ++Index)
        {
            RenderSequence(Roots[Index], FString(), FString(), false);
        }
    }
    if (Document.Nodes.IsEmpty()) Output += TEXT("└─ [Empty]\n");
    return Output;
}

FString FGraphXPortFormatter::ToMermaid(const FGraphXPortDocument& Document)
{
    FString Output = TEXT("flowchart LR\n");
    TMap<FString, FString> MermaidIds;
    for (int32 Index = 0; Index < Document.Nodes.Num(); ++Index)
    {
        const FGraphXPortNode& Node = Document.Nodes[Index];
        const FString MermaidId = FString::Printf(TEXT("N%d"), Index + 1);
        MermaidIds.Add(Node.Id, MermaidId);
        Output += FString::Printf(TEXT("    %s[\"%s\"]\n"), *MermaidId, *MermaidEscape(Node.Title));
    }
    for (const FGraphXPortEdge& Edge : Document.Edges)
    {
        const FString Source = MermaidIds.FindRef(Edge.SourceNodeId);
        const FString Target = MermaidIds.FindRef(Edge.TargetNodeId);
        const TCHAR* Arrow = Edge.Kind == TEXT("execution") ? TEXT("-->") : TEXT("-.->");
        Output += FString::Printf(TEXT("    %s %s|%s| %s\n"), *Source, Arrow, *MermaidEscape(Edge.SourcePinName), *Target);
    }
    return Output;
}
