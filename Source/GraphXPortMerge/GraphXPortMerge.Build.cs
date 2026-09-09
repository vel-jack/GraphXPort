using UnrealBuildTool;

public class GraphXPortMerge : ModuleRules
{
    public GraphXPortMerge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ApplicationCore",
            "BlueprintGraph",
            "GraphEditor",
            "InputCore",
            "Kismet",
            "Projects",
            "PropertyEditor",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UnrealEd",
            "WorkspaceMenuStructure"
        });
    }
}
