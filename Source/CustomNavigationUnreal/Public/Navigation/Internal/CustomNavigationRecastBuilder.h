// Created by Stalker7274

#pragma once

/**
 * INTERNAL: exported only for automation tests. Not a stable API.
 * The header contains no Recast types; Recast is used only in the .cpp.
 */

#include "CoreMinimal.h"
#include "Navigation/Internal/CustomNavigationSnapshot.h"
#include "Containers/Queue.h"
#include <atomic>

/** Thread-safe cooperative cancellation flag shared by the coordinator and one job. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavCancellation
{
	std::atomic<bool> bCancelled{false};

	void Cancel() { bCancelled.store(true, std::memory_order_relaxed); }
	bool IsCancelled() const { return bCancelled.load(std::memory_order_relaxed); }
};

using FCustomNavCancellationRef = TSharedRef<FCustomNavCancellation, ESPMode::ThreadSafe>;

/** Output of one build. Plain owned data. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavBuildResult
{
	FCustomNavBuildToken Token;
	ECustomNavBuildStatus Status = ECustomNavBuildStatus::None;
	FCustomNavDiagnostics Diagnostics;
	/** Set only when Status == Success. */
	TSharedPtr<FCustomNavRegionMesh, ESPMode::ThreadSafe> Mesh;
	double TotalBuildMs = 0.0;
};

/**
 * Finished builds of one world, waiting for the game thread. The build thread pushes, the subsystem pops in its tick.
 * Jobs hold a shared reference, so a job that outlives its world still has somewhere to put its result; nobody reads it
 * and it is freed with the last reference.
 */
struct FCustomNavBuildCompletionQueue
{
	TQueue<FCustomNavBuildResult, EQueueMode::Mpsc> Results;
};

/**
 * Runs the Recast solo-mesh pipeline on a snapshot:
 * heightfield -> slope classification + areas -> rasterize -> filters -> compact heightfield -> erode ->
 * distance field -> watershed regions -> contours -> poly mesh -> (detail mesh) -> copy to FCustomNavRegionMesh.
 *
 * Pure function: touches only the snapshot, its own Recast allocations and a per-build rcContext.
 * Safe to call from any thread, and concurrently with other builds (no global Recast state is changed).
 * Cancellation is checked between stages; a stage already running is not interrupted.
 */
class CUSTOMNAVIGATIONUNREAL_API FCustomNavRecastBuilder
{
public:
	static FCustomNavBuildResult Build(const FCustomNavBuildSnapshot& Snapshot, const FCustomNavCancellation* Cancellation = nullptr);
};
