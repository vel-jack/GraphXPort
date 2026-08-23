#include "GraphXPortSerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"

namespace
{
    FString GuidString(const FGuid& Guid, const FString& Fallback)
    {
        return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : Fallback;
    }

    FString ContainerTypeString(const EPinContainerType ContainerType)
    {
        switch (ContainerType)
        {
        case EPinContainerType::Array: return TEXT("array");
        case EPinContainerType::Set: return TEXT("set");
        case EPinContainerType::Map: return TEXT("map");
        default: return TEXT("none");
        }
    }

    FString ObjectPath(const UObject* Object)
    {
        return Object != nullptr ? Object->GetPathName() : FString();
    }

    void SetMemberReference(FGraphXPortNode& OutNode, const FMemberReference& Reference)
    {
        OutNode.MemberName = Reference.GetMemberName().ToString();
        OutNode.MemberParentPath = ObjectPath(Reference.GetMemberParentClass());
    }

    FGraphXPortPin SerializePin(const UEdGraphPin* Pin, const int32 NodePinOrder)
    {
        FGraphXPortPin Result;
        Result.NodePinOrder = NodePinOrder;
        Result.Id = GuidString(Pin->PinId, Pin->PinName.ToString());
        Result.Name = Pin->PinName.ToString();
        Result.Direction = Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
        Result.Category = Pin->PinType.PinCategory.ToString();
        Result.SubCategory = Pin->PinType.PinSubCategory.ToString();
        Result.ContainerType = ContainerTypeString(Pin->PinType.ContainerType);
        Result.TypeObjectPath = ObjectPath(Pin->PinType.PinSubCategoryObject.Get());
        Result.DefaultValue = Pin->DefaultValue;
        Result.DefaultObjectPath = ObjectPath(Pin->DefaultObject);
        Result.DefaultText = Pin->DefaultTextValue.IsEmpty() ? FString() : Pin->DefaultTextValue.ToString();
        Result.bHidden = Pin->bHidden;
        Result.bAdvancedView = Pin->bAdvancedView;
        Result.bOrphaned = Pin->bOrphanedPin;
        return Result;
    }

    FGraphXPortNode SerializeNode(const UEdGraphNode* Node)
    {
        FGraphXPortNode Result;
        Result.Id = GuidString(Node->NodeGuid, Node->GetName());
        Result.Name = Node->GetName();
        Result.ClassPath = Node->GetClass()->GetPathName();
        Result.Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        Result.Comment = Node->NodeComment;
        Result.PositionX = Node->NodePosX;
        Result.PositionY = Node->NodePosY;

        if (const UK2Node_CallFunction* FunctionNode = Cast<UK2Node_CallFunction>(Node))
        {
            SetMemberReference(Result, FunctionNode->FunctionReference);
        }
        else if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
        {
            SetMemberReference(Result, VariableNode->VariableReference);
        }
        else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
        {
            SetMemberReference(Result, EventNode->EventReference);
        }
        else if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
        {
            SetMemberReference(Result, EntryNode->FunctionReference);
        }
        else if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
        {
            Result.MacroGraphPath = ObjectPath(MacroNode->GetMacroGraph());
        }

        for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
        {
            const UEdGraphPin* Pin = Node->Pins[PinIndex];
            if (Pin != nullptr)
            {
                Result.Pins.Add(SerializePin(Pin, PinIndex));
            }
        }
        Result.Pins.Sort([](const FGraphXPortPin& A, const FGraphXPortPin& B)
        {
            return A.Id < B.Id;
        });
        return Result;
    }

    FString EdgeKey(const FGraphXPortEdge& Edge)
    {
        return Edge.SourceNodeId + TEXT("|") + Edge.SourcePinId + TEXT("|") + Edge.TargetNodeId + TEXT("|") + Edge.TargetPinId;
    }

    FString ExternalKey(const FGraphXPortExternalLink& Link)
    {
        return Link.LocalNodeId + TEXT("|") + Link.LocalPinId + TEXT("|") + Link.ExternalNodeId + TEXT("|") + Link.ExternalPinId;
    }

    bool BuildDocument(
        const UBlueprint* Blueprint,
        const UEdGraph* Graph,
        const TArray<const UEdGraphNode*>& InputNodes,
        const FString& Scope,
        FGraphXPortDocument& OutDocument,
        FString& OutError)
    {
        if (Blueprint == nullptr || Graph == nullptr)
        {
            OutError = TEXT("Blueprint and graph are required.");
            return false;
        }

        TSet<const UEdGraphNode*> IncludedNodes;
        for (const UEdGraphNode* Node : InputNodes)
        {
            if (Node != nullptr && Node->GetGraph() == Graph)
            {
                IncludedNodes.Add(Node);
            }
        }
        if (IncludedNodes.IsEmpty())
        {
            OutError = TEXT("No Blueprint graph nodes were selected.");
            return false;
        }

        OutDocument = FGraphXPortDocument();
        OutDocument.BlueprintPath = Blueprint->GetPathName();
        OutDocument.BlueprintName = Blueprint->GetName();
        OutDocument.GraphName = Graph->GetName();
        OutDocument.GraphId = GuidString(Graph->GraphGuid, Graph->GetPathName());
        OutDocument.Scope = Scope;

        TSet<FString> EdgeKeys;
        TSet<FString> ExternalKeys;
        for (const UEdGraphNode* Node : IncludedNodes)
        {
            OutDocument.Nodes.Add(SerializeNode(Node));
            const FString LocalNodeId = GuidString(Node->NodeGuid, Node->GetName());
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr) continue;
                const FString LocalPinId = GuidString(Pin->PinId, Pin->PinName.ToString());
                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin == nullptr || LinkedPin->GetOwningNode() == nullptr) continue;
                    const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
                    const FString LinkedNodeId = GuidString(LinkedNode->NodeGuid, LinkedNode->GetName());
                    const FString LinkedPinId = GuidString(LinkedPin->PinId, LinkedPin->PinName.ToString());

                    if (IncludedNodes.Contains(LinkedNode))
                    {
                        const UEdGraphPin* SourcePin = Pin;
                        const UEdGraphPin* TargetPin = LinkedPin;
                        const UEdGraphNode* SourceNode = Node;
                        const UEdGraphNode* TargetNode = LinkedNode;
                        if (Pin->Direction == EGPD_Input && LinkedPin->Direction == EGPD_Output)
                        {
                            Swap(SourcePin, TargetPin);
                            Swap(SourceNode, TargetNode);
                        }
                        FGraphXPortEdge Edge;
                        Edge.SourceNodeId = GuidString(SourceNode->NodeGuid, SourceNode->GetName());
                        Edge.SourcePinId = GuidString(SourcePin->PinId, SourcePin->PinName.ToString());
                        Edge.SourcePinName = SourcePin->PinName.ToString();
                        Edge.TargetNodeId = GuidString(TargetNode->NodeGuid, TargetNode->GetName());
                        Edge.TargetPinId = GuidString(TargetPin->PinId, TargetPin->PinName.ToString());
                        Edge.TargetPinName = TargetPin->PinName.ToString();
                        Edge.Kind = SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("execution") : TEXT("data");
                        const FString Key = EdgeKey(Edge);
                        if (!EdgeKeys.Contains(Key))
                        {
                            EdgeKeys.Add(Key);
                            OutDocument.Edges.Add(MoveTemp(Edge));
                        }
                    }
                    else
                    {
                        FGraphXPortExternalLink Link;
                        Link.LocalNodeId = LocalNodeId;
                        Link.LocalPinId = LocalPinId;
                        Link.LocalPinName = Pin->PinName.ToString();
                        Link.Direction = Pin->Direction == EGPD_Input ? TEXT("incoming") : TEXT("outgoing");
                        Link.ExternalNodeId = LinkedNodeId;
                        Link.ExternalNodeTitle = LinkedNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
                        Link.ExternalNodeClassPath = LinkedNode->GetClass()->GetPathName();
                        Link.ExternalPinId = LinkedPinId;
                        Link.ExternalPinName = LinkedPin->PinName.ToString();
                        const FString Key = ExternalKey(Link);
                        if (!ExternalKeys.Contains(Key))
                        {
                            ExternalKeys.Add(Key);
                            OutDocument.ExternalLinks.Add(MoveTemp(Link));
                        }
                    }
                }
            }
        }

        OutDocument.Nodes.Sort([](const FGraphXPortNode& A, const FGraphXPortNode& B) { return A.Id < B.Id; });
        OutDocument.Edges.Sort([](const FGraphXPortEdge& A, const FGraphXPortEdge& B) { return EdgeKey(A) < EdgeKey(B); });
        OutDocument.ExternalLinks.Sort([](const FGraphXPortExternalLink& A, const FGraphXPortExternalLink& B)
        {
            return ExternalKey(A) < ExternalKey(B);
        });
        return true;
    }
}

bool FGraphXPortSerializer::BuildSelection(
    const UBlueprint* Blueprint,
    const UEdGraph* Graph,
    const TArray<const UEdGraphNode*>& Nodes,
    FGraphXPortDocument& OutDocument,
    FString& OutError)
{
    return BuildDocument(Blueprint, Graph, Nodes, TEXT("selection"), OutDocument, OutError);
}

bool FGraphXPortSerializer::BuildGraph(
    const UBlueprint* Blueprint,
    const UEdGraph* Graph,
    FGraphXPortDocument& OutDocument,
    FString& OutError)
{
    TArray<const UEdGraphNode*> Nodes;
    for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
    {
        if (Node != nullptr) Nodes.Add(Node);
    }
    return BuildDocument(Blueprint, Graph, Nodes, TEXT("graph"), OutDocument, OutError);
}
