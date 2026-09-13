#include "SGraphXPortDiffPanel.h"

#include "DetailsDiff.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "IDetailsView.h"
#include "SCSDiff.h"
#include "SKismetInspector.h"
#include "SMyBlueprint.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

void SGraphXPortDiffPanel::EnsureInspector(FSideData& Side)
{
    if (!Side.Blueprint || Side.Inspector.IsValid()) return;
    // This is the public, standalone diff construction path used by SBlueprintDiff.
    // With no FBlueprintEditor supplied, My Blueprint has no authoring commands.
    Side.MyBlueprint = SNew(SMyBlueprint, TWeakPtr<FBlueprintEditor>(), Side.Blueprint.Get());
    Side.Inspector = SNew(SKismetInspector)
        .HideNameArea(true)
        .ViewIdentifier(FName("GraphXPortInspector"))
        .MyBlueprintWidget(Side.MyBlueprint)
        .IsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([] { return false; }))
        .ShowLocalVariables(true);
    Side.MyBlueprint->SetInspector(Side.Inspector);
}

void SGraphXPortDiffPanel::ReleaseSideWidgets()
{
    for (FSideData* Side : { &BaseSide, &MineSide, &TheirsSide, &ResultSide })
    {
        Side->Details.Reset();
        Side->Components.Reset();
        Side->MyBlueprint.Reset();
        Side->Inspector.Reset();
    }
}

TSharedRef<SWidget> SGraphXPortDiffPanel::BuildMembersWidget(FSideData& Side)
{
    if (!Side.Blueprint)
        return SNew(STextBlock).Text(LOCTEXT("NoBlueprint", "Blueprint is not available in this revision."));

    if (CurrentView == EGraphXPortView::MyBlueprint)
    {
        EnsureInspector(Side);
        Side.MyBlueprint->SetFocusedGraph(FindGraphForSide(Side, SelectedGraphName));
        Side.MyBlueprint->Refresh();
        Side.MyBlueprint->ClearGraphActionMenuSelection();
        Side.Inspector->ShowDetailsForObjects(TArray<UObject*>());
        if (!InspectedMemberName.IsNone())
        {
            Side.MyBlueprint->SelectItemByName(InspectedMemberName, ESelectInfo::OnNavigation);
        }
        return SNew(SSplitter).Orientation(Orient_Vertical)
            + SSplitter::Slot().Value(0.65f) [ Side.MyBlueprint.ToSharedRef() ]
            + SSplitter::Slot().Value(0.35f) [ Side.Inspector.ToSharedRef() ];
    }

    if (CurrentView == EGraphXPortView::Components && InspectedComponentName.IsNone())
    {
        // FSCSDiff assumes an actor CDO. Interfaces, libraries and failed historical loads
        // must get an explicit empty state rather than dereferencing a missing class.
        if (!Side.Blueprint->GeneratedClass ||
            !Side.Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()) ||
            !Side.Blueprint->SimpleConstructionScript)
        {
            return SNew(STextBlock).Text(LOCTEXT("NoComponentTree", "No component hierarchy in this Blueprint."));
        }
        if (!Side.Components.IsValid()) Side.Components = MakeShared<FSCSDiff>(Side.Blueprint.Get());
        return Side.Components->TreeWidget();
    }

    UObject* Object = nullptr;
    if (CurrentView == EGraphXPortView::ClassSettings)
    {
        Object = Side.Blueprint.Get();
    }
    else if (CurrentView == EGraphXPortView::Components)
    {
        if (Side.Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Side.Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->GetVariableName() == InspectedComponentName)
                {
                    Object = Node->ComponentTemplate;
                    break;
                }
            }
        }
    }
    else
    {
        Object = const_cast<UObject*>(DiffUtils::GetCDO(Side.Blueprint.Get()));
    }

    if (!Object)
        return SNew(STextBlock).Text(LOCTEXT("MissingMemberObject", "This member is not available in this revision."));

    // Native diff customizations provide the same class options/default categories as UE.
    Side.Details = FDetailsDiff::CreateDetailsView(Object);
    const bool bEditable = Side.bEditable && CurrentView != EGraphXPortView::ClassSettings;
    Side.Details->SetIsPropertyEditingEnabledDelegate(
        FIsPropertyEditingEnabled::CreateLambda([bEditable] { return bEditable; }));
    Side.Details->HighlightProperty(InspectedProperty.ResolvePath(Object));
    return Side.Details.ToSharedRef();
}

#undef LOCTEXT_NAMESPACE
