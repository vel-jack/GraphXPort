#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Algo/Reverse.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Tunnel.h"
#include "GraphXPortFormatter.h"
#include "GraphXPortSerializer.h"

namespace
{
    struct FAsciiFixture
    {
        FGraphXPortDocument Doc;
        FAsciiFixture() { Doc.BlueprintName = TEXT("BP"); Doc.GraphName = TEXT("Run"); }
        int32 Node(const FString& Name, const FString& Class = TEXT("K2Node_CallFunction"))
        {
            const int32 Index = Doc.Nodes.AddDefaulted();
            FGraphXPortNode& Result = Doc.Nodes[Index];
            Result.Id = FString::Printf(TEXT("%08x-node"), Index + 1);
            Result.Title = Name;
            Result.Name = Name;
            Result.ClassPath = Class;
            return Index;
        }
        void Pin(int32 NodeIndex, const FString& Name, const FString& Direction,
            const FString& Category = TEXT("exec"), const FString& Default = FString())
        {
            FGraphXPortNode& Owner = Doc.Nodes[NodeIndex];
            FGraphXPortPin P;
            P.Id = Owner.Id + TEXT(".") + Name;
            P.Name = Name;
            P.Direction = Direction;
            P.Category = Category;
            P.DefaultValue = Default;
            P.ContainerType = TEXT("none");
            P.NodePinOrder = Owner.Pins.Num();
            Owner.Pins.Add(P);
        }
        const FGraphXPortPin& GetPin(int32 NodeIndex, const FString& Name) const
        {
            return *Doc.Nodes[NodeIndex].Pins.FindByPredicate([&](const FGraphXPortPin& P) { return P.Name == Name; });
        }
        void Edge(int32 From, const FString& Out, int32 To, const FString& In, const FString& Kind = TEXT("execution"))
        {
            FGraphXPortEdge E;
            E.SourceNodeId = Doc.Nodes[From].Id;
            E.SourcePinId = GetPin(From, Out).Id;
            E.SourcePinName = Out;
            E.TargetNodeId = Doc.Nodes[To].Id;
            E.TargetPinId = GetPin(To, In).Id;
            E.TargetPinName = In;
            E.Kind = Kind;
            Doc.Edges.Add(E);
        }
        void External(int32 Local, const FString& PinName, const FString& Direction,
            const FString& ExternalId, const FString& ExternalPin)
        {
            FGraphXPortExternalLink E;
            E.LocalNodeId = Doc.Nodes[Local].Id;
            E.LocalPinId = GetPin(Local, PinName).Id;
            E.LocalPinName = PinName;
            E.Direction = Direction;
            E.ExternalNodeId = ExternalId;
            E.ExternalNodeTitle = TEXT("Outside");
            E.ExternalPinId = ExternalId + TEXT(".") + ExternalPin;
            E.ExternalPinName = ExternalPin;
            Doc.ExternalLinks.Add(E);
        }
        FString Text() const { return FGraphXPortFormatter::Format(Doc, EGraphXPortFormat::Ascii); }
        void Permute()
        {
            Algo::Reverse(Doc.Nodes);
            Algo::Reverse(Doc.Edges);
            Algo::Reverse(Doc.ExternalLinks);
            for (FGraphXPortNode& N : Doc.Nodes) Algo::Reverse(N.Pins);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortAsciiGoldenTest, "GraphXPort.Format.Ascii.Golden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortAsciiGoldenTest::RunTest(const FString& Parameters)
{
    FAsciiFixture F;
    const int32 B = F.Node(TEXT("Branch"), TEXT("K2Node_IfThenElse"));
    F.Pin(B, TEXT("execute"), TEXT("input"));
    F.Pin(B, TEXT("then"), TEXT("output"));
    F.Pin(B, TEXT("else"), TEXT("output"));
    const int32 A = F.Node(TEXT("Action"));
    F.Pin(A, TEXT("execute"), TEXT("input"));
    F.Edge(B, TEXT("then"), A, TEXT("execute"));
    const FString Golden = TEXT("BP :: Run\n")
        TEXT("└─ [00000001] Branch\n")
        TEXT("   ├─ TRUE → [00000002] Action\n")
        TEXT("   │  └─ [End]\n")
        TEXT("   └─ FALSE → [Unconnected]\n");
    TestEqual(TEXT("A single connected Branch path remains labelled; unconnected FALSE is explicit"), F.Text(), Golden);
    F.Permute();
    TestEqual(TEXT("Tree is unchanged by document array permutations"), F.Text(), Golden);

    FAsciiFixture Plain;
    const int32 Action = Plain.Node(TEXT("Action"));
    Plain.Pin(Action, TEXT("execute"), TEXT("input"));
    Plain.Pin(Action, TEXT("then"), TEXT("output"));
    Plain.External(Action, TEXT("then"), TEXT("outgoing"), TEXT("aaaaaaaa-external"), TEXT("Break"));
    TestTrue(TEXT("Ordinary then is not mislabelled TRUE"), Plain.Text().Contains(TEXT("then → [aaaaaaaa]")));
    TestTrue(TEXT("Special destination input is preserved"), Plain.Text().Contains(TEXT("[in: Break]")));
    TestFalse(TEXT("Generic then must not become TRUE"), Plain.Text().Contains(TEXT("TRUE →")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortAsciiCoverageTest, "GraphXPort.Format.Ascii.CoverageAndReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortAsciiCoverageTest::RunTest(const FString& Parameters)
{
    FAsciiFixture F;
    for (const TCHAR* Name : {TEXT("FirstEvent"), TEXT("SecondEvent")})
    {
        const int32 Event = F.Node(Name, TEXT("K2Node_CustomEvent"));
        F.Pin(Event, TEXT("then"), TEXT("output"));
        const int32 Action = F.Node(FString(Name) + TEXT("Action"));
        F.Pin(Action, TEXT("execute"), TEXT("input"));
        F.Edge(Event, TEXT("then"), Action, TEXT("execute"));
    }
    const int32 CycleA = F.Node(TEXT("CycleA"));
    const int32 CycleB = F.Node(TEXT("CycleB"));
    for (int32 N : {CycleA, CycleB})
    {
        F.Pin(N, TEXT("execute"), TEXT("input"));
        F.Pin(N, TEXT("then"), TEXT("output"));
    }
    F.Edge(CycleA, TEXT("then"), CycleB, TEXT("execute"));
    F.Edge(CycleB, TEXT("then"), CycleA, TEXT("execute"));
    const int32 Pure = F.Node(TEXT("Compute"));
    F.Pin(Pure, TEXT("First"), TEXT("input"), TEXT("int"), TEXT("1"));
    F.Pin(Pure, TEXT("Second"), TEXT("input"), TEXT("int"), TEXT("2"));
    F.Pin(Pure, TEXT("X"), TEXT("output"), TEXT("int"));
    F.Pin(Pure, TEXT("Y"), TEXT("output"), TEXT("int"));
    const int32 Sink = F.Node(TEXT("Sink"));
    F.Pin(Sink, TEXT("execute"), TEXT("input"));
    F.Pin(Sink, TEXT("A"), TEXT("input"), TEXT("int"));
    F.Pin(Sink, TEXT("B"), TEXT("input"), TEXT("int"));
    F.Edge(Pure, TEXT("X"), Sink, TEXT("A"), TEXT("data"));
    F.Edge(Pure, TEXT("Y"), Sink, TEXT("B"), TEXT("data"));
    const FString Before = F.Text();
    TestTrue(TEXT("Both event roots and actions are included"), Before.Contains(TEXT("FirstEventAction"))
        && Before.Contains(TEXT("SecondEventAction")));
    TestTrue(TEXT("Rootless cycle is represented and terminates"), Before.Contains(TEXT("CycleA"))
        && Before.Contains(TEXT("CycleB")) && Before.Contains(TEXT("↩")));
    TestTrue(TEXT("Pure call arguments are ordered/named and X output identified"), Before.Contains(TEXT("Compute(First=1, Second=2).X")));
    TestTrue(TEXT("Different output of the same call is identified"), Before.Contains(TEXT("Compute(First=1, Second=2).Y")));
    F.Permute();
    TestEqual(TEXT("Events, cycles and expressions are permutation invariant"), F.Text(), Before);

    FAsciiFixture Data;
    const int32 Value = Data.Node(TEXT("Value"), TEXT("K2Node_VariableGet"));
    Data.Pin(Value, TEXT("Value"), TEXT("output"), TEXT("int"));
    const int32 Consumer = Data.Node(TEXT("PureConsumer"));
    Data.Pin(Consumer, TEXT("Input"), TEXT("input"), TEXT("int"));
    Data.Pin(Consumer, TEXT("Result"), TEXT("output"), TEXT("int"));
    Data.Edge(Value, TEXT("Value"), Consumer, TEXT("Input"), TEXT("data"));
    TestTrue(TEXT("Pure-only selection is not empty"), Data.Text().Contains(TEXT("PureConsumer [Data]")));
    TestTrue(TEXT("Pure-only selection retains upstream pin"), Data.Text().Contains(TEXT("Input ← [00000001].Value")));

    FAsciiFixture DataCycle;
    const int32 DA = DataCycle.Node(TEXT("DataA"));
    const int32 DB = DataCycle.Node(TEXT("DataB"));
    for (int32 N : {DA, DB})
    {
        DataCycle.Pin(N, TEXT("Value"), TEXT("input"), TEXT("int"));
        DataCycle.Pin(N, TEXT("Result"), TEXT("output"), TEXT("int"));
    }
    DataCycle.Edge(DA, TEXT("Result"), DB, TEXT("Value"), TEXT("data"));
    DataCycle.Edge(DB, TEXT("Result"), DA, TEXT("Value"), TEXT("data"));
    TestTrue(TEXT("Data cycle terminates with an explicit marker"), DataCycle.Text().Contains(TEXT("[Data cycle]")));
    FAsciiFixture Empty;
    TestEqual(TEXT("Empty canonical document renders explicitly"), Empty.Text(), FString(TEXT("BP :: Run\n└─ [Empty]\n")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortAsciiBoundaryTest, "GraphXPort.Format.Ascii.ReroutesAndBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortAsciiBoundaryTest::RunTest(const FString& Parameters)
{
    FAsciiFixture F;
    const int32 Start = F.Node(TEXT("Start"));
    F.Doc.Nodes[Start].Id = TEXT("12345678-aaaa-start");
    F.Pin(Start, TEXT("execute"), TEXT("input"));
    F.Pin(Start, TEXT("then"), TEXT("output"));
    F.Pin(Start, TEXT("Value"), TEXT("input"), TEXT("int"));
    const int32 ExecKnot = F.Node(TEXT("Reroute"), TEXT("K2Node_Knot"));
    F.Pin(ExecKnot, TEXT("InputPin"), TEXT("input"));
    F.Pin(ExecKnot, TEXT("OutputPin"), TEXT("output"));
    F.Edge(Start, TEXT("then"), ExecKnot, TEXT("InputPin"));
    F.External(ExecKnot, TEXT("OutputPin"), TEXT("outgoing"), TEXT("12345678-bbbb-external"), TEXT("Break"));
    const int32 DataKnot = F.Node(TEXT("DataReroute"), TEXT("K2Node_Knot"));
    F.Pin(DataKnot, TEXT("InputPin"), TEXT("input"), TEXT("int"));
    F.Pin(DataKnot, TEXT("OutputPin"), TEXT("output"), TEXT("int"));
    F.External(DataKnot, TEXT("InputPin"), TEXT("incoming"), TEXT("12345678-bbbb-external"), TEXT("Amount"));
    F.Edge(DataKnot, TEXT("OutputPin"), Start, TEXT("Value"), TEXT("data"));
    const FString Before = F.Text();
    TestTrue(TEXT("Internal/external short-ID collisions are resolved together"), Before.Contains(TEXT("[12345678-aaa] Start"))
        && Before.Contains(TEXT("[12345678-bbb]")));
    TestTrue(TEXT("Incoming data follows reroute to external pin"), Before.Contains(TEXT("Value ← [12345678-bbb].Amount [External]")));
    TestTrue(TEXT("Outgoing execution follows reroute to external input"), Before.Contains(TEXT("[in: Break] [External]")));
    TestFalse(TEXT("Connected reroutes are hidden"), Before.Contains(TEXT("Reroute")));
    F.Permute();
    TestEqual(TEXT("Boundary rendering is permutation invariant"), F.Text(), Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortAsciiEngineNodesTest, "GraphXPort.Format.Ascii.EngineControlNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortAsciiEngineNodesTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("ControlGraph"), RF_Transient);
    Graph->Schema = UEdGraphSchema_K2::StaticClass();
    Blueprint->UbergraphPages.Add(Graph);
    const auto Add = [Graph](UEdGraphNode* Node)
    {
        Graph->AddNode(Node, false, false);
        Node->CreateNewGuid();
    };
    UK2Node_ExecutionSequence* Sequence = NewObject<UK2Node_ExecutionSequence>(Graph);
    Add(Sequence);
    Sequence->AllocateDefaultPins();
    Sequence->AddInputPin();
    UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
    Add(Branch);
    Branch->AllocateDefaultPins();
    UK2Node_IfThenElse* NestedBranch = NewObject<UK2Node_IfThenElse>(Graph);
    Add(NestedBranch);
    NestedBranch->AllocateDefaultPins();
    UK2Node_SwitchString* Switch = NewObject<UK2Node_SwitchString>(Graph);
    Add(Switch);
    Switch->PinNames = {FName(TEXT("Then")), FName(TEXT("Idle_Case"))};
    Switch->AllocateDefaultPins();

    // Real macro instances, with transient tunnel definitions matching loop interface pins.
    const auto MakeLoop = [&](const TCHAR* Name)
    {
        UBlueprint* Library = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
        UEdGraph* Macro = NewObject<UEdGraph>(Library, Name, RF_Transient);
        Macro->Schema = UEdGraphSchema_K2::StaticClass();
        Library->MacroGraphs.Add(Macro);
        UK2Node_Tunnel* Entry = NewObject<UK2Node_Tunnel>(Macro);
        Macro->AddNode(Entry, false, false);
        Entry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("execute"));
        UK2Node_Tunnel* Exit = NewObject<UK2Node_Tunnel>(Macro);
        Macro->AddNode(Exit, false, false);
        Exit->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("LoopBody"));
        Exit->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("Completed"));
        UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(Graph);
        Add(Instance);
        Instance->SetMacroGraph(Macro);
        Instance->AllocateDefaultPins();
        return Instance;
    };
    UK2Node_MacroInstance* ForLoop = MakeLoop(TEXT("ForLoop"));
    UK2Node_MacroInstance* ForEach = MakeLoop(TEXT("ForEachLoop"));
    UEdGraphPin* LoopBody = ForLoop->FindPin(TEXT("LoopBody"));
    UEdGraphPin* EachInput = ForEach->FindPin(TEXT("execute"));
    if (!TestNotNull(TEXT("Loop macro body pin exists"), LoopBody)
        || !TestNotNull(TEXT("ForEach macro execution pin exists"), EachInput)) return false;
    Sequence->GetThenPinGivenIndex(0)->MakeLinkTo(Branch->GetExecPin());
    Sequence->GetThenPinGivenIndex(1)->MakeLinkTo(ForLoop->FindPin(TEXT("execute")));
    Sequence->GetThenPinGivenIndex(2)->MakeLinkTo(Switch->GetExecPin());
    Branch->GetThenPin()->MakeLinkTo(NestedBranch->GetExecPin());
    NestedBranch->GetElsePin()->MakeLinkTo(Switch->GetExecPin());
    LoopBody->MakeLinkTo(EachInput);

    FGraphXPortDocument Doc;
    FString Error;
    if (!TestTrue(TEXT("Real editor nodes serialize"), FGraphXPortSerializer::BuildGraph(Blueprint, Graph, Doc, Error))) return false;
    const FString Text = FGraphXPortFormatter::Format(Doc, EGraphXPortFormat::Ascii);
    const int32 Then0 = Text.Find(TEXT("THEN 0 →"));
    const int32 Then1 = Text.Find(TEXT("THEN 1 →"));
    const int32 Then2 = Text.Find(TEXT("THEN 2 →"));
    TestTrue(TEXT("Sequence output order survives GUID sorting"), Then0 != INDEX_NONE && Then1 > Then0 && Then2 > Then1);
    TestTrue(TEXT("Nested Branch paths retain both labels"), Text.Contains(TEXT("TRUE →")) && Text.Contains(TEXT("FALSE →")));
    TestTrue(TEXT("Loop macro outputs receive loop labels"), Text.Contains(TEXT("LOOP BODY →")) && Text.Contains(TEXT("COMPLETED →")));
    TestTrue(TEXT("Switch case Then is not rewritten to TRUE"), Text.Contains(TEXT("Then →")));
    TestTrue(TEXT("Switch case spelling/underscores preserved"), Text.Contains(TEXT("Idle_Case →")));
    TestTrue(TEXT("Switch default is explicit"), Text.Contains(TEXT("DEFAULT →")));

    TArray<const UEdGraphNode*> Selection;
    for (const TObjectPtr<UEdGraphNode>& Node : Graph->Nodes) Selection.Add(Node.Get());
    FGraphXPortDocument A;
    FGraphXPortDocument B;
    FGraphXPortSerializer::BuildSelection(Blueprint, Graph, Selection, A, Error);
    Algo::Reverse(Selection);
    Algo::Reverse(Graph->Nodes);
    for (const TObjectPtr<UEdGraphNode>& Node : Graph->Nodes)
        for (UEdGraphPin* Pin : Node->Pins)
            if (Pin) Algo::Reverse(Pin->LinkedTo);
    FGraphXPortSerializer::BuildSelection(Blueprint, Graph, Selection, B, Error);
    TestEqual(TEXT("Serializer canonical JSON is independent of selection, graph-array and link order"),
        FGraphXPortFormatter::Format(A, EGraphXPortFormat::Json), FGraphXPortFormatter::Format(B, EGraphXPortFormat::Json));
    TestEqual(TEXT("Serializer tree is independent of selection order"),
        FGraphXPortFormatter::Format(A, EGraphXPortFormat::Ascii), FGraphXPortFormatter::Format(B, EGraphXPortFormat::Ascii));
    FGraphXPortDocument ReorderedGraph;
    FGraphXPortSerializer::BuildGraph(Blueprint, Graph, ReorderedGraph, Error);
    TestEqual(TEXT("Whole-graph JSON is independent of graph-array and link order"),
        FGraphXPortFormatter::Format(Doc, EGraphXPortFormat::Json), FGraphXPortFormatter::Format(ReorderedGraph, EGraphXPortFormat::Json));
    return true;
}

#endif
