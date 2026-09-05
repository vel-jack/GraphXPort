#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GraphXPortBeautifier.h"

namespace
{
    struct FBeautifierFixture
    {
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;

        FBeautifierFixture()
        {
            Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
            Graph = NewObject<UEdGraph>(Blueprint, NAME_None, RF_Transient);
            Graph->Schema = UEdGraphSchema_K2::StaticClass();
        }

        UEdGraphNode* MakeNode(const int32 PosX, const int32 PosY)
        {
            UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transient);
            Graph->AddNode(Node, false, false);
            Node->CreateNewGuid();
            Node->NodePosX = PosX;
            Node->NodePosY = PosY;
            return Node;
        }

        static UEdGraphPin* AddExec(UEdGraphNode* Node, const EEdGraphPinDirection Direction, const FName Name)
        {
            return Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Exec, Name);
        }

        static UEdGraphPin* AddData(UEdGraphNode* Node, const EEdGraphPinDirection Direction, const FName Name)
        {
            return Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Int, Name);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortBeautifierLinearTest, "GraphXPort.Beautify.LinearChainAndBranch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortBeautifierLinearTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FBeautifierFixture F;

    UEdGraphNode* A = F.MakeNode(0, 0);
    UEdGraphNode* B = F.MakeNode(0, 0);
    UEdGraphNode* C = F.MakeNode(0, 0);
    FBeautifierFixture::AddExec(A, EGPD_Output, TEXT("then"))->MakeLinkTo(FBeautifierFixture::AddExec(B, EGPD_Input, TEXT("execute")));
    FBeautifierFixture::AddExec(B, EGPD_Output, TEXT("then"))->MakeLinkTo(FBeautifierFixture::AddExec(C, EGPD_Input, TEXT("execute")));

    TArray<UEdGraphNode*> Selection{A, B, C};
    TMap<UEdGraphNode*, FVector2D> Positions;
    TestTrue(TEXT("Linear chain lays out"), FGraphXPortBeautifier::ComputeLayout(Selection, Positions));
    TestEqual(TEXT("All three nodes are positioned"), Positions.Num(), 3);
    TestTrue(TEXT("Chain strictly increases in X (layer) order"),
        Positions[A].X < Positions[B].X && Positions[B].X < Positions[C].X);
    TestTrue(TEXT("A straight 1-out/1-in chain shares one row so the wire is horizontal"),
        Positions[A].Y == Positions[B].Y && Positions[B].Y == Positions[C].Y);

    // Branch: both arms should land in the same next layer (same X), at different rows (Y).
    UEdGraphNode* Branch = F.MakeNode(0, 0);
    UEdGraphNode* TrueArm = F.MakeNode(0, 0);
    UEdGraphNode* FalseArm = F.MakeNode(0, 0);
    FBeautifierFixture::AddExec(Branch, EGPD_Input, TEXT("execute"));
    UEdGraphPin* Then = FBeautifierFixture::AddExec(Branch, EGPD_Output, TEXT("then"));
    UEdGraphPin* Else = FBeautifierFixture::AddExec(Branch, EGPD_Output, TEXT("else"));
    Then->MakeLinkTo(FBeautifierFixture::AddExec(TrueArm, EGPD_Input, TEXT("execute")));
    Else->MakeLinkTo(FBeautifierFixture::AddExec(FalseArm, EGPD_Input, TEXT("execute")));

    TArray<UEdGraphNode*> BranchSelection{Branch, TrueArm, FalseArm};
    TMap<UEdGraphNode*, FVector2D> BranchPositions;
    TestTrue(TEXT("Branch selection lays out"), FGraphXPortBeautifier::ComputeLayout(BranchSelection, BranchPositions));
    TestEqual(TEXT("Both arms share the branch's next layer"), BranchPositions[TrueArm].X, BranchPositions[FalseArm].X);
    TestTrue(TEXT("Arms are in a later layer than the branch"), BranchPositions[TrueArm].X > BranchPositions[Branch].X);
    TestNotEqual(TEXT("Arms occupy different rows"), BranchPositions[TrueArm].Y, BranchPositions[FalseArm].Y);

    // A merge point (two incoming exec edges) has no single sole source, so it must not
    // be treated as a straight pass-through of either arm - it gets its own layer, one
    // past both arms, rather than aliasing onto either arm's chain.
    UEdGraphNode* Merge = F.MakeNode(0, 0);
    FBeautifierFixture::AddExec(Merge, EGPD_Input, TEXT("execute"));
    FBeautifierFixture::AddExec(TrueArm, EGPD_Output, TEXT("then"))->MakeLinkTo(Merge->Pins[0]);
    FBeautifierFixture::AddExec(FalseArm, EGPD_Output, TEXT("then"))->MakeLinkTo(Merge->Pins[0]);

    TArray<UEdGraphNode*> MergeSelection{Branch, TrueArm, FalseArm, Merge};
    TMap<UEdGraphNode*, FVector2D> MergePositions;
    TestTrue(TEXT("Branch-then-merge selection lays out"), FGraphXPortBeautifier::ComputeLayout(MergeSelection, MergePositions));
    TestTrue(TEXT("Merge lands one layer past both arms"),
        MergePositions[Merge].X > MergePositions[TrueArm].X && MergePositions[Merge].X > MergePositions[FalseArm].X);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortBeautifierEdgeCasesTest, "GraphXPort.Beautify.CyclesAndIslands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortBeautifierEdgeCasesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    {
        // Rootless execution cycle must terminate and place both nodes.
        FBeautifierFixture F;
        UEdGraphNode* CycleA = F.MakeNode(0, 0);
        UEdGraphNode* CycleB = F.MakeNode(0, 0);
        FBeautifierFixture::AddExec(CycleA, EGPD_Input, TEXT("execute"));
        FBeautifierFixture::AddExec(CycleB, EGPD_Input, TEXT("execute"));
        UEdGraphPin* AOut = FBeautifierFixture::AddExec(CycleA, EGPD_Output, TEXT("then"));
        UEdGraphPin* BOut = FBeautifierFixture::AddExec(CycleB, EGPD_Output, TEXT("then"));
        AOut->MakeLinkTo(CycleB->Pins[0]);
        BOut->MakeLinkTo(CycleA->Pins[0]);

        TArray<UEdGraphNode*> Selection{CycleA, CycleB};
        TMap<UEdGraphNode*, FVector2D> Positions;
        TestTrue(TEXT("Cyclic selection still lays out without hanging"), FGraphXPortBeautifier::ComputeLayout(Selection, Positions));
        TestEqual(TEXT("Both cycle nodes are positioned"), Positions.Num(), 2);
    }
    {
        // Two disconnected islands should each be laid out independently, not merged into one column.
        FBeautifierFixture F;
        UEdGraphNode* IslandA1 = F.MakeNode(0, 0);
        UEdGraphNode* IslandA2 = F.MakeNode(0, 0);
        FBeautifierFixture::AddExec(IslandA1, EGPD_Output, TEXT("then"))
            ->MakeLinkTo(FBeautifierFixture::AddExec(IslandA2, EGPD_Input, TEXT("execute")));
        UEdGraphNode* IslandB1 = F.MakeNode(0, 200);
        UEdGraphNode* IslandB2 = F.MakeNode(0, 200);
        FBeautifierFixture::AddExec(IslandB1, EGPD_Output, TEXT("then"))
            ->MakeLinkTo(FBeautifierFixture::AddExec(IslandB2, EGPD_Input, TEXT("execute")));

        TArray<UEdGraphNode*> Selection{IslandA1, IslandA2, IslandB1, IslandB2};
        TMap<UEdGraphNode*, FVector2D> Positions;
        TestTrue(TEXT("Disconnected islands lay out"), FGraphXPortBeautifier::ComputeLayout(Selection, Positions));
        TestEqual(TEXT("All four nodes positioned"), Positions.Num(), 4);
        TestEqual(TEXT("Island A root starts at layer 0"), Positions[IslandA1].X, Positions[IslandB1].X);
        TestTrue(TEXT("Each island's second node is one layer ahead of its own root"),
            Positions[IslandA2].X > Positions[IslandA1].X && Positions[IslandB2].X > Positions[IslandB1].X);
    }
    {
        // Pure/data-only node should be placed one layer before its consumer, not
        // crammed into the consumer's own column.
        FBeautifierFixture F;
        UEdGraphNode* Sink = F.MakeNode(0, 0);
        FBeautifierFixture::AddExec(Sink, EGPD_Input, TEXT("execute"));
        UEdGraphPin* SinkInput = FBeautifierFixture::AddData(Sink, EGPD_Input, TEXT("A"));
        UEdGraphNode* Pure = F.MakeNode(0, 0);
        UEdGraphPin* PureOutput = FBeautifierFixture::AddData(Pure, EGPD_Output, TEXT("Result"));
        PureOutput->MakeLinkTo(SinkInput);

        TArray<UEdGraphNode*> Selection{Sink, Pure};
        TMap<UEdGraphNode*, FVector2D> Positions;
        TestTrue(TEXT("Pure/consumer pair lays out"), FGraphXPortBeautifier::ComputeLayout(Selection, Positions));
        TestTrue(TEXT("Pure node lands one layer before its consumer"), Positions[Pure].X < Positions[Sink].X);
    }
    {
        // Fewer than two nodes: nothing meaningful to arrange.
        FBeautifierFixture F;
        UEdGraphNode* Only = F.MakeNode(0, 0);
        TArray<UEdGraphNode*> Selection{Only};
        TMap<UEdGraphNode*, FVector2D> Positions;
        TestFalse(TEXT("Single-node selection reports no layout"), FGraphXPortBeautifier::ComputeLayout(Selection, Positions));
    }
    return true;
}

#endif
