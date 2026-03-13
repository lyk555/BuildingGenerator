using System.IO;
using UnrealBuildTool;

public class BuildingGenerator : ModuleRules
{
    public BuildingGenerator(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "PCG" });

        // Clipper2 headers
        string Clipper2Include = Path.Combine(ModuleDirectory, "ThirdParty", "Clipper2", "include");
        PublicSystemIncludePaths.Add(Clipper2Include);

        // Fix C4668: make the macro defined
        PublicDefinitions.Add("CLIPPER2_HI_PRECISION=0");

        // Fix C4800: don't treat warnings as errors in this module
        bWarningsAsErrors = false;  // module property
    }
}
