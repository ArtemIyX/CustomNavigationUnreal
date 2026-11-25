// Copyright Epic Games, Inc. All Rights Reserved.]

#include "RecastThirdParty/Public/RecastThirdParty.h"

#define LOCTEXT_NAMESPACE "FRecastThirdPartyModule"

void FRecastThirdPartyModule::StartupModule()
{
	IModuleInterface::StartupModule();
}

void FRecastThirdPartyModule::ShutdownModule()
{
	IModuleInterface::ShutdownModule();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRecastThirdPartyModule, RecastThirdParty)