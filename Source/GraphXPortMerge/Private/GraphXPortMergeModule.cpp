#include "GraphXPortMergeModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GraphXPortTempPackage.h"
#include "SGraphXPortMergeWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "GraphXPortMerge"

namespace GraphXPortMergeTabIds
{
    static const FName MergeWindow(TEXT("GraphXPortMerge"));
}

void FGraphXPortMergeModule::StartupModule()
{
    FGraphXPortTempPackage::CleanStaleTempFiles();

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        GraphXPortMergeTabIds::MergeWindow,
        FOnSpawnTab::CreateRaw(this, &FGraphXPortMergeModule::SpawnMergeTab))
        .SetDisplayName(LOCTEXT("MergeTabTitle", "GraphXPort Merge"))
        .SetTooltipText(LOCTEXT("MergeTabTooltip", "Resolve Blueprint git conflicts side by side."))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsStructureRoot())
        .SetIcon(FSlateIcon());
}

void FGraphXPortMergeModule::ShutdownModule()
{
    if (FSlateApplication::IsInitialized())
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GraphXPortMergeTabIds::MergeWindow);
    }
}

TSharedRef<SDockTab> FGraphXPortMergeModule::SpawnMergeTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        .Label(LOCTEXT("MergeTabTitle", "GraphXPort Merge"))
        [
            SNew(SGraphXPortMergeWindow)
        ];
}

IMPLEMENT_MODULE(FGraphXPortMergeModule, GraphXPortMerge)

#undef LOCTEXT_NAMESPACE
