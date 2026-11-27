// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Containers/Ticker.h"
#include "Navigation/CustomNavigationTypes.h"
#include "Navigation/CustomNavigationGeometryProvider.h"
#include "CustomNavigationWorldSubsystem.generated.h"

struct FCustomNavBuildResult;
struct FCustomNavBuildToken;
struct FCustomNavCancellation;
struct FCustomNavBuildCompletionQueue;

/**
 * Where build work runs. The default queues it on the dedicated "CustomNavBuild" FRunnable thread.
 * The work itself pushes its result into the world's completion queue; nothing is posted back to the game thread.
 * Tests replace it to hold a job and run it later deterministically.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavBuildExecutor
{
	TFunction<void(TUniqueFunction<void()>&& Work)> LaunchWorker;

	static TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> MakeDefault();
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCustomNavRegionBuildFinished, const FCustomNavRegionKey& /*Key*/, const FCustomNavRegionStatus& /*Status*/);

/**
 * Per-world coordinator of custom (Recast) navmesh builds. Separate from Unreal's own navigation system.
 *
 * Threading:
 *  - Every public function must be called on the game thread (checked).
 *  - Providers are invoked on the game thread during collection; the result is frozen into an immutable snapshot.
 *  - Only the snapshot goes to the build thread. It never touches the world, providers or any UObject.
 *  - The build thread pushes the result into this world's completion queue. The subsystem drains it on the game
 *    thread in its ticker, within a time budget (CustomNav.CompletionBudgetMs). A result is published only if the
 *    subsystem is not closing and the job's token (world lifetime, key, desired revision, request id) still matches.
 *    Otherwise it is discarded.
 *
 * Scheduling: one active job per world. Requests and invalidations only mark a region dirty; the build starts on
 * the next ticker update (or FlushPendingBuilds), so many changes in one frame produce one build. The ticker runs
 * only while there is pending work, a running job or an undrained result.
 */
UCLASS()
class CUSTOMNAVIGATIONUNREAL_API UCustomNavigationWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UCustomNavigationWorldSubsystem* Get(const UWorld* World);

	// UWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// --- Providers ---

	/**
	 * Registers a geometry provider for this world. Higher Priority is collected first and wins source-key conflicts.
	 * The subsystem keeps a shared reference until UnregisterGeometryProvider or world teardown.
	 * Invalidates all regions.
	 */
	FCustomNavGeometryProviderHandle RegisterGeometryProvider(TSharedRef<ICustomNavGeometryProvider> Provider, int32 Priority = 0);
	/** Unregisters a provider. Invalidates all regions. Unknown handles are ignored. */
	void UnregisterGeometryProvider(FCustomNavGeometryProviderHandle Handle);
	int32 NumGeometryProviders() const { return Providers.Num(); }

	// --- Invalidation ---

	/** Marks every region intersecting Bounds dirty. For a move pass both old and new bounds (call twice). */
	void InvalidateGeometry(const FBox& Bounds);
	/** Marks every region dirty. */
	void InvalidateAll();

	// --- Builds ---

	/**
	 * Requests a build of a region. If an identical request (same key, bounds and settings) is already ready or in
	 * progress, nothing new is started: compatible consumers share one build and one result.
	 * @return the desired revision the caller can wait for (status.PublishedRevision >= it), 0 if rejected.
	 */
	uint64 RequestRegionBuild(const FCustomNavRegionRequest& Request);
	/** Forgets a region and its published mesh. A running job for it is cancelled and its result discarded. */
	void RemoveRegion(const FCustomNavRegionKey& Key);

	FCustomNavRegionStatus GetRegionStatus(const FCustomNavRegionKey& Key) const;
	bool HasRegion(const FCustomNavRegionKey& Key) const { return Regions.Contains(Key); }
	TArray<FCustomNavRegionKey> GetRegionKeys() const;

	/**
	 * Published immutable mesh. By default only a current mesh is returned; pass bAllowStale = true to also get the
	 * last good mesh while a rebuild is pending or after a failure (check status.bPublishedIsStale).
	 */
	FCustomNavRegionMeshPtr GetPublishedRegion(const FCustomNavRegionKey& Key, bool bAllowStale = false) const;

	/** Starts pending builds now instead of waiting for the ticker. Normally not needed. */
	void FlushPendingBuilds();

	/**
	 * Applies finished builds from the completion queue now instead of waiting for the ticker.
	 * Stops once BudgetSeconds has been spent (negative = no limit), but always applies at least one result.
	 * @return how many results were taken from the queue (published or discarded).
	 */
	int32 ProcessCompletedBuilds(double BudgetSeconds = -1.0);

	bool IsBuildRunning() const { return ActiveRequestId != 0; }
	bool IsClosing() const { return bClosing; }

	/** Fired on the game thread after a build of the region finished (published, failed or rejected). Not fired for discarded completions. */
	FOnCustomNavRegionBuildFinished OnRegionBuildFinished;

	// --- Test hooks ---

	void SetBuildExecutorForTesting(TSharedPtr<FCustomNavBuildExecutor, ESPMode::ThreadSafe> InExecutor);
	/** Disables the automatic ticker so tests decide when FlushPendingBuilds runs. */
	void SetAutoScheduleForTesting(bool bEnabled) { bAutoSchedule = bEnabled; }
	uint64 GetWorldLifetimeId() const { return WorldLifetimeId; }
	int32 GetLaunchedJobCount() const { return LaunchedJobCount; }

private:
	struct FProviderEntry
	{
		FCustomNavGeometryProviderHandle Handle;
		TSharedPtr<ICustomNavGeometryProvider> Provider;
		int32 Priority = 0;
		uint64 RegistrationOrder = 0;
	};

	struct FRegionRecord
	{
		FCustomNavRegionKey Key;
		FBox WorldBounds = FBox(ForceInit);
		FCustomNavBuildSettings Settings;
		uint64 DesiredRevision = 0;
		uint64 PublishedRevision = 0;
		bool bPending = false;
		uint64 RunningRequestId = 0;
		TSharedPtr<FCustomNavCancellation, ESPMode::ThreadSafe> RunningCancellation;
		ECustomNavBuildStatus LastOutcome = ECustomNavBuildStatus::None;
		uint64 LastOutcomeRevision = 0;
		FCustomNavDiagnostics LastDiagnostics;
		/** Collection/validation diagnostics of the running job, merged with the worker's on completion. */
		FCustomNavDiagnostics RunningCollectDiagnostics;
		FCustomNavRegionMeshPtr Published;
		int32 DiscardedCompletions = 0;
	};

	bool TickScheduler(float DeltaTime);
	void EnsureTicker();
	bool HasUndrainedResults() const;
	void OnBuildCompleted(FCustomNavBuildResult&& Result);
	void MarkDirty(FRegionRecord& Record);
	void EnqueuePending(const FCustomNavRegionKey& Key);
	/**
	 * Collects, validates and dispatches one region. Returns true if a job was launched.
	 * Takes the key, not a record reference: providers run inside and may re-enter the subsystem.
	 */
	bool TryStartBuild(const FCustomNavRegionKey& Key);
	void FinishWithoutWorker(FRegionRecord& Record, ECustomNavBuildStatus Status, FCustomNavDiagnostics&& Diagnostics);
	FCustomNavRegionStatus MakeStatus(const FRegionRecord& Record) const;
	ECustomNavRegionState ComputeState(const FRegionRecord& Record) const;

	TArray<FProviderEntry> Providers;
	TMap<FCustomNavRegionKey, FRegionRecord> Regions;
	TArray<FCustomNavRegionKey> PendingQueue;

	TSharedPtr<FCustomNavBuildExecutor, ESPMode::ThreadSafe> Executor;
	/** Shared with running jobs; dropped in Deinitialize so late results go nowhere. */
	TSharedPtr<FCustomNavBuildCompletionQueue, ESPMode::ThreadSafe> Completions;
	FTSTicker::FDelegateHandle TickerHandle;

	uint64 WorldLifetimeId = 0;
	uint64 NextProviderId = 1;
	uint64 NextRequestId = 1;
	uint64 RegistryGeneration = 0;
	uint64 ActiveRequestId = 0;
	FCustomNavRegionKey ActiveKey;
	int32 LaunchedJobCount = 0;
	bool bClosing = false;
	bool bAutoSchedule = true;
};
