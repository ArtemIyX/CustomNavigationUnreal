using System.IO;
using UnrealBuildTool;

public class RecastThirdParty : ModuleRules
{
    public RecastThirdParty(ReadOnlyTargetRules Target) : base(Target)
    {
        //PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        
        PublicDependencyModuleNames.AddRange(new string[]
        {
	        
        });
        
	    PrivateDependencyModuleNames.AddRange(new string[] 
		{
			"Core",
			"CoreUObject",
			"Engine"
        });
	    
	    PublicIncludePaths.AddRange( new string[]
	    {
		    Path.Combine(ModuleDirectory, "Public/Detour"),
		    Path.Combine(ModuleDirectory, "Public/Recast")
	    });
    }
}