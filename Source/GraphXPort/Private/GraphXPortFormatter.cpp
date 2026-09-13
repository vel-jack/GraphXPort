#include "GraphXPortFormatter.h"
#include "GraphXPortAsciiFormatter.h"

namespace
{
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

}

FString FGraphXPortFormatter::Format(const FGraphXPortDocument& Document, const EGraphXPortFormat FormatType)
{
    switch (FormatType)
    {
    case EGraphXPortFormat::AiText: return ToAiText(Document);
    case EGraphXPortFormat::Ascii: return ToAscii(Document);
    case EGraphXPortFormat::Mermaid: return ToMermaid(Document);
    default: return ToAiText(Document);
    }
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
    return FGraphXPortAsciiFormatter::Format(Document);
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
