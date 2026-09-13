#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SGraphXPortDiffPanel.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphXPortNavigationTest, "GraphXPort.Merge.Navigation.BlueprintReview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGraphXPortNavigationTest::RunTest(const FString& Parameters)
{
    const TSharedRef<SGraphXPortDiffPanel> Panel = SNew(SGraphXPortDiffPanel);
    UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
    auto Graph = [Blueprint](const TCHAR* Name)
    {
        UEdGraph* Result = NewObject<UEdGraph>(Blueprint, FName(Name), RF_Transient);
        Result->Schema = UEdGraphSchema_K2::StaticClass();
        return Result;
    };
    Blueprint->UbergraphPages.Add(Graph(TEXT("EventGraph")));
    Blueprint->FunctionGraphs.Add(Graph(TEXT("UserConstructionScript")));
    Blueprint->FunctionGraphs.Add(Graph(TEXT("WalkableFunction")));
    Blueprint->MacroGraphs.Add(Graph(TEXT("WalkableMacro")));
    FBPVariableDescription Variable;
    Variable.VarName = "Walkable";
    Variable.VarGuid = FGuid::NewGuid();
    Variable.VarType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    Blueprint->NewVariables.Add(Variable);

    Panel->ResultSide.Asset = Blueprint;
    Panel->ResultSide.Blueprint = Blueprint;
    Panel->Mode = EGraphXPortDiffMode::NewFile;
    Panel->CollectGraphNames();
    Panel->ComputeAllDiffs();
    Panel->BuildDiffTree();
    TestEqual(TEXT("Four native sections and all four graphs are reachable without a base"), Panel->DiffTreeRoots.Num(), 8);
    TestEqual(TEXT("No-base placeholders are not changes"), Panel->VisibleDifferences.Num(), 0);
    TestTrue(TEXT("My Blueprint is the first destination"), Panel->DiffTreeRoots[0]->View == EGraphXPortView::MyBlueprint);
    TestTrue(TEXT("Defaults have their own destination"), Panel->DiffTreeRoots[1]->View == EGraphXPortView::Defaults);
    TestTrue(TEXT("Class settings have their own destination"), Panel->DiffTreeRoots[2]->View == EGraphXPortView::ClassSettings);
    TestTrue(TEXT("Components have their own destination"), Panel->DiffTreeRoots[3]->View == EGraphXPortView::Components);
    TSet<FName> Graphs;
    for (const auto& Root : Panel->DiffTreeRoots)
    {
        if (Root->View == EGraphXPortView::Graph)
        {
            TestFalse(TEXT("No duplicate graph navigation"), Graphs.Contains(Root->GraphName));
            Graphs.Add(Root->GraphName);
        }
    }
    TestTrue(TEXT("Function appears even with no changed nodes"), Graphs.Contains("WalkableFunction"));
    TestTrue(TEXT("Macro appears even with no changed nodes"), Graphs.Contains("WalkableMacro"));

    // The filter uses the same row identities and does not create a second Changes tree.
    auto Change = MakeShared<FGraphXPortDiffTreeNode>();
    Change->Label = FText::FromString(TEXT("WORKING: Added variable 'Walkable'"));
    Change->View = EGraphXPortView::MyBlueprint;
    Change->MemberName = "Walkable";
    Change->bIsDifference = true;
    Panel->DiffTreeRoots[0]->Children.Add(Change);
    Panel->bChangesOnly = true;
    Panel->FilterNavigation();
    TestEqual(TEXT("Changes-only hides unchanged sections"), Panel->VisibleTreeRoots.Num(), 1);
    TestEqual(TEXT("Changes-only excludes placeholder leaves"), Panel->VisibleDifferences.Num(), 1);
    TestTrue(TEXT("Filtered change retains identity"), Panel->VisibleDifferences[0] == Change);
    Panel->NavigationFilter = TEXT("not-present");
    Panel->FilterNavigation();
    TestTrue(TEXT("Unmatched search has no results"), Panel->VisibleTreeRoots.IsEmpty());
    Panel->NavigationFilter = TEXT("Walkable");
    Panel->FilterNavigation();
    TestEqual(TEXT("Member search finds the change's section"), Panel->VisibleTreeRoots.Num(), 1);

    Panel->ReleaseLoadedRevisions();
    TestTrue(TEXT("Release clears navigation references"), Panel->VisibleTreeRoots.IsEmpty());
    TestTrue(TEXT("Release clears change references"), Panel->VisibleDifferences.IsEmpty());

    // Exercise the real native widgets with generated transient classes, never user assets.
    UBlueprint* NativeBlueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), "GraphXPortReviewFixture"),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Native pane fixture"), NativeBlueprint)) return false;
    Panel->BaseSide.Asset = NativeBlueprint;
    Panel->BaseSide.Blueprint = NativeBlueprint;
    Panel->ResultSide.Asset = NativeBlueprint;
    Panel->ResultSide.Blueprint = NativeBlueprint;
    Panel->Mode = EGraphXPortDiffMode::ReadOnlyCompare;
    Panel->NavigationFilter.Empty();
    Panel->bChangesOnly = false;
    Panel->CollectGraphNames();
    Panel->ComputeAllDiffs();
    Panel->BuildDiffTree();
    TestEqual(TEXT("Identical class settings and graphs have no changes"), Panel->VisibleDifferences.Num(), 0);
    for (const auto& Root : Panel->DiffTreeRoots)
    {
        Panel->OnTreeSelectionChanged(Root, ESelectInfo::Direct);
        TestTrue(TEXT("Selection routes to its native destination"), Panel->CurrentView == Root->View);
    }
    Panel->ReleaseLoadedRevisions();
    return true;
}

#endif
