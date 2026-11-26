// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CustomNavigationUnreal : ModuleRules
{
	public CustomNavigationUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(new string[]
		{
				"Core",
				"CoreUObject",
				"Engine",
		});
			
		
		PrivateDependencyModuleNames.AddRange(new string[]
		{
				"RenderCore",
				"RHI",
				"Slate",
				"SlateCore",
				"RecastThirdParty"
		});
	}
}
