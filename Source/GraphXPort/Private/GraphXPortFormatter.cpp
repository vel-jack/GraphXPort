#include "GraphXPortFormatter.h"
#include "GraphXPortAsciiFormatter.h"

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
