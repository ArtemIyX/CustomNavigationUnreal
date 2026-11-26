// Copyright Epic Games, Inc. All Rights Reserved.

#include "CustomNavigationUnreal.h"
#include "Navigation/CustomNavigationTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <atomic>

#define LOCTEXT_NAMESPACE "FCustomNavigationUnrealModule"

namespace CustomNavigationPrivate
{
	extern std::atomic<int32> GInFlightWorkerJobs; // CustomNavigationWorldSubsystem.cpp
}

void FCustomNavigationUnrealModule::StartupModule()
{
}

void FCustomNavigationUnrealModule::ShutdownModule()
{
	// Worker jobs run code from this module; wait for them before it can be unloaded.
	// Their completions are dropped: subsystems are gone by now.
	const double Deadline = FPlatformTime::Seconds() + 30.0;
	while (CustomNavigationPrivate::GInFlightWorkerJobs.load() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	if (CustomNavigationPrivate::GInFlightWorkerJobs.load() > 0)
	{
		UE_LOG(LogCustomNavigation, Error, TEXT("Module shutdown with %d navmesh build jobs still running"), CustomNavigationPrivate::GInFlightWorkerJobs.load());
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCustomNavigationUnrealModule, CustomNavigationUnreal)