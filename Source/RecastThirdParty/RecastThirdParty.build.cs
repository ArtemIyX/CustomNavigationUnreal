using System.IO;
using UnrealBuildTool;

public class RecastThirdParty : ModuleRules
{
    public RecastThirdParty(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        
        PublicDependencyModuleNames.AddRange(new string[]
        {
	        
	        
        });
        
	    PrivateDependencyModuleNames.AddRange(new string[] 
		{
			
			
        });
	    
	    PublicIncludePaths.AddRange( new string[]
	    {
		    Path.Combine(ModuleDirectory, "Detour/Include"),
		    Path.Combine(ModuleDirectory, "Recast/Include")
	    });
    }
}