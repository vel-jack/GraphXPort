#include "GraphXPortModule.h"

#include "EdGraph/EdGraphSchema.h"
#include "GraphXPortMenu.h"
#include "ToolMenu.h"
#include "ToolMenus.h"

void FGraphXPortModule::StartupModule()
{
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGraphXPortModule::RegisterMenus));
}

void FGraphXPortModule::ShutdownModule()
{
    if (UToolMenus::IsToolMenuUIEnabled())
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }
}

void FGraphXPortModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    const FName BaseSchemaMenu = UEdGraphSchema::GetContextMenuName(UEdGraphSchema::StaticClass());
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(BaseSchemaMenu);
    Menu->AddDynamicSection(TEXT("GraphXPortDynamic"), FNewToolMenuDelegate::CreateStatic(&FGraphXPortMenu::AddContextSection));
}

IMPLEMENT_MODULE(FGraphXPortModule, GraphXPort)
