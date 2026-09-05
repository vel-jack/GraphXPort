#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GraphXPortFormatter.h"
#include "GraphXPortSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FGraphXPortExportTest,
    "GraphXPort.Export.SelectionAndWholeGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortExportTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), TEXT("BP_GraphXPortTest"));
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("TestGraph"));
    Blueprint->FunctionGraphs.Add(Graph);

    UEdGraphNode* First = NewObject<UEdGraphNode>(Graph, TEXT("First"));
    UEdGraphNode* Second = NewObject<UEdGraphNode>(Graph, TEXT("Second"));
    UEdGraphNode* External = NewObject<UEdGraphNode>(Graph, TEXT("External"));
    Graph->AddNode(First, true, false);
    Graph->AddNode(Second, true, false);
    Graph->AddNode(External, true, false);
    First->CreateNewGuid();
    Second->CreateNewGuid();
    External->CreateNewGuid();

    UEdGraphPin* FirstThen = First->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("Then"));
    UEdGraphPin* SecondExecute = Second->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("Execute"));
    UEdGraphPin* SecondValue = Second->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Object, TEXT("Value"));
    UEdGraphPin* ExternalValue = External->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, TEXT("Value"));
    FirstThen->MakeLinkTo(SecondExecute);
    SecondValue->MakeLinkTo(ExternalValue);

    FGraphXPortDocument Selection;
    FString Error;
    const TArray<const UEdGraphNode*> SelectedNodes{First, Second};
    TestTrue(TEXT("Selection export succeeds"),
        FGraphXPortSerializer::BuildSelection(Blueprint, Graph, SelectedNodes, Selection, Error));
    TestEqual(TEXT("Selection contains two nodes"), Selection.Nodes.Num(), 2);
    TestEqual(TEXT("Selection contains one internal edge"), Selection.Edges.Num(), 1);
    TestEqual(TEXT("Selection records one external link"), Selection.ExternalLinks.Num(), 1);

    const FString JsonA = FGraphXPortFormatter::Format(Selection, EGraphXPortFormat::Json);
    const FString JsonB = FGraphXPortFormatter::Format(Selection, EGraphXPortFormat::Json);
    TestEqual(TEXT("JSON is deterministic"), JsonA, JsonB);
    TestTrue(TEXT("JSON identifies GraphXPort"), JsonA.Contains(TEXT("\"generator\": \"GraphXPort\"")));
    TestTrue(TEXT("JSON declares canonical format version 1"), JsonA.Contains(TEXT("\"formatVersion\": 1")));
    TestTrue(TEXT("JSON preserves node-relative pin order"), JsonA.Contains(TEXT("\"nodePinOrder\": 0")));

    const FString Mermaid = FGraphXPortFormatter::Format(Selection, EGraphXPortFormat::Mermaid);
    TestTrue(TEXT("Mermaid output declares a flowchart"), Mermaid.StartsWith(TEXT("flowchart LR")));
    TestTrue(TEXT("Mermaid output includes an execution edge"), Mermaid.Contains(TEXT("-->")));

    const FString Ascii = FGraphXPortFormatter::Format(Selection, EGraphXPortFormat::Ascii);
    TestTrue(TEXT("ASCII output uses compact tree connectors"), Ascii.Contains(TEXT("├─")));
    TestTrue(TEXT("ASCII output includes traceable short node IDs"),
        Ascii.Contains(First->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower).Left(8)));
    TestFalse(TEXT("ASCII output is not the old edge list"), Ascii.Contains(TEXT(" ==> ")));

    FGraphXPortDocument ControlTree;
    ControlTree.BlueprintName = TEXT("BP_ControlTree");
    ControlTree.GraphName = TEXT("ChoosePath");
    ControlTree.Nodes.Reserve(6);

    const auto AddNode = [&ControlTree](const FString& Id, const FString& Title, const FString& ClassPath) -> FGraphXPortNode&
    {
        FGraphXPortNode& Node = ControlTree.Nodes.AddDefaulted_GetRef();
        Node.Id = Id;
        Node.Name = Title;
        Node.Title = Title;
        Node.ClassPath = ClassPath;
        return Node;
    };
    const auto AddPin = [](FGraphXPortNode& Node, const FString& Id, const FString& Name,
        const FString& Direction, const FString& Category, const int32 Order, const FString& DefaultValue = FString())
    {
        FGraphXPortPin& Pin = Node.Pins.AddDefaulted_GetRef();
        Pin.Id = Id;
        Pin.Name = Name;
        Pin.Direction = Direction;
        Pin.Category = Category;
        Pin.ContainerType = TEXT("none");
        Pin.NodePinOrder = Order;
        Pin.DefaultValue = DefaultValue;
    };
    const auto AddEdge = [&ControlTree](const FString& SourceNode, const FString& SourcePin, const FString& SourceName,
        const FString& TargetNode, const FString& TargetPin, const FString& TargetName, const FString& Kind)
    {
        FGraphXPortEdge& Edge = ControlTree.Edges.AddDefaulted_GetRef();
        Edge.SourceNodeId = SourceNode;
        Edge.SourcePinId = SourcePin;
        Edge.SourcePinName = SourceName;
        Edge.TargetNodeId = TargetNode;
        Edge.TargetPinId = TargetPin;
        Edge.TargetPinName = TargetName;
        Edge.Kind = Kind;
    };

    FGraphXPortNode& Entry = AddNode(TEXT("aaaaaaaa-entry"), TEXT("ChoosePath"), TEXT("K2Node_FunctionEntry"));
    Entry.MemberName = TEXT("ChoosePath");
    AddPin(Entry, TEXT("entry-exec"), TEXT("then"), TEXT("output"), TEXT("exec"), 0);
    AddPin(Entry, TEXT("entry-entity"), TEXT("Entity"), TEXT("output"), TEXT("object"), 1);

    FGraphXPortNode& Branch = AddNode(TEXT("12345678-aaaa-branch"), TEXT("Branch"), TEXT("K2Node_IfThenElse"));
    AddPin(Branch, TEXT("branch-in"), TEXT("execute"), TEXT("input"), TEXT("exec"), 0);
    AddPin(Branch, TEXT("branch-then"), TEXT("then"), TEXT("output"), TEXT("exec"), 1);
    AddPin(Branch, TEXT("branch-else"), TEXT("else"), TEXT("output"), TEXT("exec"), 2);

    FGraphXPortNode& TrueNode = AddNode(TEXT("12345678-bbbb-true"), TEXT("True Action"), TEXT("K2Node_CallFunction"));
    TrueNode.MemberName = TEXT("TrueAction");
    AddPin(TrueNode, TEXT("true-in"), TEXT("execute"), TEXT("input"), TEXT("exec"), 0);
    AddPin(TrueNode, TEXT("true-out"), TEXT("then"), TEXT("output"), TEXT("exec"), 1);
    AddPin(TrueNode, TEXT("true-entity"), TEXT("Entity"), TEXT("input"), TEXT("object"), 2);

    FGraphXPortNode& FalseNode = AddNode(TEXT("bbbbbbbb-false"), TEXT("False Action"), TEXT("K2Node_CallFunction"));
    FalseNode.MemberName = TEXT("FalseAction");
    AddPin(FalseNode, TEXT("false-in"), TEXT("execute"), TEXT("input"), TEXT("exec"), 0);
    AddPin(FalseNode, TEXT("false-out"), TEXT("then"), TEXT("output"), TEXT("exec"), 1);
    AddPin(FalseNode, TEXT("false-selected"), TEXT("Selected"), TEXT("input"), TEXT("bool"), 2, TEXT("false"));

    FGraphXPortNode& Merge = AddNode(TEXT("cccccccc-merge"), TEXT("Continue"), TEXT("K2Node_CallFunction"));
    Merge.MemberName = TEXT("Continue");
    AddPin(Merge, TEXT("merge-in"), TEXT("execute"), TEXT("input"), TEXT("exec"), 0);

    AddEdge(Entry.Id, TEXT("entry-exec"), TEXT("then"), Branch.Id, TEXT("branch-in"), TEXT("execute"), TEXT("execution"));
    // Deliberately add ELSE before TRUE; the formatter must use node pin order.
    AddEdge(Branch.Id, TEXT("branch-else"), TEXT("else"), FalseNode.Id, TEXT("false-in"), TEXT("execute"), TEXT("execution"));
    AddEdge(Branch.Id, TEXT("branch-then"), TEXT("then"), TrueNode.Id, TEXT("true-in"), TEXT("execute"), TEXT("execution"));
    AddEdge(TrueNode.Id, TEXT("true-out"), TEXT("then"), Merge.Id, TEXT("merge-in"), TEXT("execute"), TEXT("execution"));
    AddEdge(FalseNode.Id, TEXT("false-out"), TEXT("then"), Merge.Id, TEXT("merge-in"), TEXT("execute"), TEXT("execution"));
    AddEdge(Entry.Id, TEXT("entry-entity"), TEXT("Entity"), TrueNode.Id, TEXT("true-entity"), TEXT("Entity"), TEXT("data"));

    const FString ControlAscii = FGraphXPortFormatter::Format(ControlTree, EGraphXPortFormat::Ascii);
    TestTrue(TEXT("ASCII header includes Blueprint and typed function identity"),
        ControlAscii.StartsWith(TEXT("BP_ControlTree :: ChoosePath(Entity: object)")));
    TestTrue(TEXT("Colliding eight-character IDs expand until unambiguous"),
        ControlAscii.Contains(TEXT("[12345678-aaa] Branch")) && ControlAscii.Contains(TEXT("[12345678-bbb] TrueAction")));
    const int32 TruePath = ControlAscii.Find(TEXT("TRUE →"));
    const int32 FalsePath = ControlAscii.Find(TEXT("FALSE →"));
    TestTrue(TEXT("Control paths preserve node pin order"), TruePath != INDEX_NONE && FalsePath > TruePath);
    TestTrue(TEXT("Connected data includes the entry pin reference"), ControlAscii.Contains(TEXT("Entity ← [aaaaaaaa].Entity")));
    TestTrue(TEXT("Literal defaults use equals"), ControlAscii.Contains(TEXT("Selected = false")));
    TestTrue(TEXT("Converging execution emits a revisit marker"), ControlAscii.Contains(TEXT("↩ [cccccccc] Continue")));

    FGraphXPortDocument WholeGraph;
    TestTrue(TEXT("Whole graph export succeeds"), FGraphXPortSerializer::BuildGraph(Blueprint, Graph, WholeGraph, Error));
    TestEqual(TEXT("Whole graph contains three nodes"), WholeGraph.Nodes.Num(), 3);
    TestEqual(TEXT("Whole graph contains two edges"), WholeGraph.Edges.Num(), 2);
    TestEqual(TEXT("Whole graph has no external links"), WholeGraph.ExternalLinks.Num(), 0);
    return true;
}

#endif
