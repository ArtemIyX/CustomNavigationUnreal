// Copyright Epic Games, Inc. All Rights Reserved.

#include "CustomNavigationUnreal.h"
#include "Navigation/CustomNavigationBuildThread.h"

#define LOCTEXT_NAMESPACE "FCustomNavigationUnrealModule"

void FCustomNavigationUnrealModule::StartupModule()
{
}

void FCustomNavigationUnrealModule::ShutdownModule()
{
	// The build thread runs code from this module: stop it and wait before the module can be unloaded.
	// Subsystems are gone by now, so their jobs are cancelled and nobody reads the results.
	FCustomNavBuildThread::Shutdown();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCustomNavigationUnrealModule, CustomNavigationUnreal)
