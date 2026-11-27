// Created by Stalker7274

#include "Navigation/CustomNavigationWorldSubsystem.h"
#include "Navigation/Internal/CustomNavigationSnapshot.h"
#include "Navigation/Internal/CustomNavigationRecastBuilder.h"
#include "Navigation/CustomNavigationBuildThread.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include <atomic>

#include UE_INLINE_GENERATED_CPP_BY_NAME(CustomNavigationWorldSubsystem)

namespace CustomNavigationPrivate
{
	static std::atomic<uint64> GNextWorldLifetimeId{1};

	static const FName StageCollectName(TEXT("Collect"));

	static float GCompletionBudgetMs = 1.0f;
	static FAutoConsoleVariableRef CVarCompletionBudgetMs(
		TEXT("CustomNav.CompletionBudgetMs"),
		GCompletionBudgetMs,
		TEXT("Game-thread time per tick for applying finished navmesh builds, per world. At least one result is always applied. <= 0: no limit."),
		ECVF_Default);
}

TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> FCustomNavBuildExecutor::MakeDefault()
{
	TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> Result = MakeShared<FCustomNavBuildExecutor, ESPMode::ThreadSafe>();
	Result->LaunchWorker = [](TUniqueFunction<void()>&& Work)
	{
		FCustomNavBuildThread::Enqueue(MoveTemp(Work));
	};
	return Result;
}

UCustomNavigationWorldSubsystem* UCustomNavigationWorldSubsystem::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UCustomNavigationWorldSubsystem>() : nullptr;
}

void UCustomNavigationWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WorldLifetimeId = CustomNavigationPrivate::GNextWorldLifetimeId.fetch_add(1);
	Executor = FCustomNavBuildExecutor::MakeDefault();
	Completions = MakeShared<FCustomNavBuildCompletionQueue, ESPMode::ThreadSafe>();
	bClosing = false;
}

void UCustomNavigationWorldSubsystem::Deinitialize()
{
	bClosing = true;

	// Running jobs keep their own copy of the snapshot and their own reference to the completion queue; cancel them.
	// Dropping our reference means their results are never read and die with the job.
	for (TPair<FCustomNavRegionKey, FRegionRecord>& Pair : Regions)
	{
		if (Pair.Value.RunningCancellation.IsValid())
		{
			Pair.Value.RunningCancellation->Cancel();
		}
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Completions.Reset();
	Regions.Empty();
	PendingQueue.Empty();
	Providers.Empty();
	ActiveRequestId = 0;
	ActiveKey = FCustomNavRegionKey();
	OnRegionBuildFinished.Clear();

	Super::Deinitialize();
}

bool UCustomNavigationWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

// --- Providers ---

FCustomNavGeometryProviderHandle UCustomNavigationWorldSubsystem::RegisterGeometryProvider(TSharedRef<ICustomNavGeometryProvider> Provider, int32 Priority)
{
	check(IsInGameThread());
	if (bClosing)
	{
		return FCustomNavGeometryProviderHandle();
	}

	FProviderEntry& Entry = Providers.AddDefaulted_GetRef();
	Entry.Handle.Id = NextProviderId++;
	Entry.Provider = Provider;
	Entry.Priority = Priority;
	Entry.RegistrationOrder = Entry.Handle.Id;
	++RegistryGeneration;

	UE_LOG(LogCustomNavigation, Verbose, TEXT("Registered provider %s (priority %d)"), *Provider->GetDebugName(), Priority);
	InvalidateAll();
	return Entry.Handle;
}

void UCustomNavigationWorldSubsystem::UnregisterGeometryProvider(FCustomNavGeometryProviderHandle Handle)
{
	check(IsInGameThread());
	if (!Handle.IsValid() || bClosing)
	{
		return;
	}
	const int32 Removed = Providers.RemoveAll([Handle](const FProviderEntry& Entry) { return Entry.Handle == Handle; });
	if (Removed > 0)
	{
		++RegistryGeneration;
		InvalidateAll();
	}
}

// --- Invalidation ---

void UCustomNavigationWorldSubsystem::InvalidateGeometry(const FBox& Bounds)
{
	check(IsInGameThread());
	if (bClosing || !Bounds.IsValid)
	{
		return;
	}
	for (TPair<FCustomNavRegionKey, FRegionRecord>& Pair : Regions)
	{
		if (Pair.Value.WorldBounds.IsValid && Pair.Value.WorldBounds.Intersect(Bounds))
		{
			MarkDirty(Pair.Value);
		}
	}
}

void UCustomNavigationWorldSubsystem::InvalidateAll()
{
	check(IsInGameThread());
	if (bClosing)
	{
		return;
	}
	for (TPair<FCustomNavRegionKey, FRegionRecord>& Pair : Regions)
	{
		MarkDirty(Pair.Value);
	}
}

void UCustomNavigationWorldSubsystem::MarkDirty(FRegionRecord& Record)
{
	++Record.DesiredRevision;
	Record.bPending = true;
	// A running job is now building an obsolete revision; stop it early, its completion will be discarded.
	if (Record.RunningCancellation.IsValid())
	{
		Record.RunningCancellation->Cancel();
	}
	EnqueuePending(Record.Key);
}

void UCustomNavigationWorldSubsystem::EnqueuePending(const FCustomNavRegionKey& Key)
{
	PendingQueue.AddUnique(Key);
	EnsureTicker();
}

void UCustomNavigationWorldSubsystem::EnsureTicker()
{
	if (bAutoSchedule && !bClosing && !TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCustomNavigationWorldSubsystem::TickScheduler));
	}
}

bool UCustomNavigationWorldSubsystem::TickScheduler(float DeltaTime)
{
	if (!bAutoSchedule || bClosing)
	{
		TickerHandle.Reset();
		return false;
	}
	// Apply results first: that frees the build slot, so the next pending region starts in the same tick.
	const float BudgetMs = CustomNavigationPrivate::GCompletionBudgetMs;
	ProcessCompletedBuilds(BudgetMs > 0.f ? BudgetMs / 1000.0 : -1.0);
	if (bClosing)
	{
		TickerHandle.Reset();
		return false;
	}
	FlushPendingBuilds();

	// Keep ticking while there is anything to start, to wait for, or to apply; new requests re-arm the ticker.
	if (PendingQueue.Num() == 0 && ActiveRequestId == 0 && !HasUndrainedResults())
	{
		TickerHandle.Reset();
		return false;
	}
	return true;
}

bool UCustomNavigationWorldSubsystem::HasUndrainedResults() const
{
	return Completions.IsValid() && !Completions->Results.IsEmpty();
}

int32 UCustomNavigationWorldSubsystem::ProcessCompletedBuilds(double BudgetSeconds)
{
	check(IsInGameThread());
	const double StartTime = FPlatformTime::Seconds();
	int32 Count = 0;
	FCustomNavBuildResult Result;
	// Re-check the queue each time: a listener may tear the subsystem down (Deinitialize drops the queue).
	while (!bClosing && Completions.IsValid() && Completions->Results.Dequeue(Result))
	{
		OnBuildCompleted(MoveTemp(Result));
		Result = FCustomNavBuildResult();
		++Count;
		if (BudgetSeconds >= 0.0 && FPlatformTime::Seconds() - StartTime >= BudgetSeconds)
		{
			break; // the rest waits for the next tick
		}
	}
	return Count;
}

// --- Builds ---

uint64 UCustomNavigationWorldSubsystem::RequestRegionBuild(const FCustomNavRegionRequest& Request)
{
	check(IsInGameThread());
	if (bClosing)
	{
		return 0;
	}
	if (Request.Key.RegionId.IsNone())
	{
		UE_LOG(LogCustomNavigation, Warning, TEXT("RequestRegionBuild: RegionId must not be None"));
		return 0;
	}

	FRegionRecord* Existing = Regions.Find(Request.Key);
	if (Existing && Existing->WorldBounds == Request.WorldBounds && Existing->Settings.Equals(Request.Settings))
	{
		const bool bInProgress = Existing->bPending || Existing->RunningRequestId != 0;
		const bool bReady = Existing->Published.IsValid() && Existing->PublishedRevision == Existing->DesiredRevision;
		if (bInProgress || bReady)
		{
			// Identical request: share the existing build/result.
			return Existing->DesiredRevision;
		}
		// Identical request after a failure (or never built): retry.
	}

	FRegionRecord& Record = Existing ? *Existing : Regions.Add(Request.Key);
	Record.Key = Request.Key;
	Record.WorldBounds = Request.WorldBounds;
	Record.Settings = Request.Settings;
	MarkDirty(Record);
	return Record.DesiredRevision;
}

void UCustomNavigationWorldSubsystem::RemoveRegion(const FCustomNavRegionKey& Key)
{
	check(IsInGameThread());
	if (FRegionRecord* Record = Regions.Find(Key))
	{
		if (Record->RunningCancellation.IsValid())
		{
			Record->RunningCancellation->Cancel();
		}
		// If its job is the active one, the slot stays busy until the (discarded) completion arrives.
		Regions.Remove(Key);
	}
	PendingQueue.Remove(Key);
}

FCustomNavRegionStatus UCustomNavigationWorldSubsystem::GetRegionStatus(const FCustomNavRegionKey& Key) const
{
	if (const FRegionRecord* Record = Regions.Find(Key))
	{
		return MakeStatus(*Record);
	}
	FCustomNavRegionStatus Status;
	Status.Key = Key;
	return Status;
}

TArray<FCustomNavRegionKey> UCustomNavigationWorldSubsystem::GetRegionKeys() const
{
	TArray<FCustomNavRegionKey> Keys;
	Regions.GetKeys(Keys);
	return Keys;
}

FCustomNavRegionMeshPtr UCustomNavigationWorldSubsystem::GetPublishedRegion(const FCustomNavRegionKey& Key, bool bAllowStale) const
{
	const FRegionRecord* Record = Regions.Find(Key);
	if (!Record || !Record->Published.IsValid())
	{
		return nullptr;
	}
	if (!bAllowStale && Record->PublishedRevision != Record->DesiredRevision)
	{
		return nullptr;
	}
	return Record->Published;
}

void UCustomNavigationWorldSubsystem::FlushPendingBuilds()
{
	check(IsInGameThread());
	while (!bClosing && ActiveRequestId == 0 && PendingQueue.Num() > 0)
	{
		const FCustomNavRegionKey Key = PendingQueue[0];
		PendingQueue.RemoveAt(0);
		const FRegionRecord* Record = Regions.Find(Key);
		if (!Record || !Record->bPending)
		{
			continue;
		}
		TryStartBuild(Key);
	}
}

bool UCustomNavigationWorldSubsystem::TryStartBuild(const FCustomNavRegionKey& Key)
{
	using namespace CustomNavigationPrivate;

	FRegionRecord* Record = Regions.Find(Key);
	if (!Record)
	{
		return false;
	}

	// Copy what the build needs: providers may re-enter the subsystem and change the map.
	const FBox Bounds = Record->WorldBounds;
	const FCustomNavBuildSettings Settings = Record->Settings;
	const uint64 DesiredRevision = Record->DesiredRevision;
	const uint64 GenerationBefore = RegistryGeneration;
	Record->bPending = false;
	Record = nullptr;

	// --- Collection (game thread) ---
	FCustomNavDiagnostics Diagnostics;
	FCustomNavGeometryCollector Collector(Diagnostics);
	FCustomNavGeometryRequest GeometryRequest;
	GeometryRequest.WorldBounds = Bounds;
	GeometryRequest.AgentProfile = Key.AgentProfile;

	TArray<FProviderEntry> Ordered = Providers;
	Ordered.StableSort([](const FProviderEntry& A, const FProviderEntry& B)
	{
		return A.Priority != B.Priority ? A.Priority > B.Priority : A.RegistrationOrder < B.RegistrationOrder;
	});

	bool bCollectFailed = false;
	for (const FProviderEntry& Entry : Ordered)
	{
		const FString Name = Entry.Provider->GetDebugName();
		Collector.SetCurrentProvider(Name, Entry.Priority);
		if (Entry.Provider->CollectGeometry(GeometryRequest, Collector, Diagnostics) == ECustomNavCollectStatus::Failed)
		{
			Diagnostics.Error(ECustomNavDiagnosticCode::ProviderFailed, StageCollectName, TEXT("Provider failed to collect geometry"), Name);
			bCollectFailed = true;
		}
	}
	bCollectFailed |= Collector.HasConflict();

	Record = Regions.Find(Key);
	if (!Record || bClosing)
	{
		return false;
	}
	if (RegistryGeneration != GenerationBefore)
	{
		Diagnostics.Warning(ECustomNavDiagnosticCode::RegistryChangedDuringCollection, StageCollectName, TEXT("Providers changed during collection; build restarted"));
	}
	if (Record->DesiredRevision != DesiredRevision)
	{
		// Something changed while collecting; the region is already queued again.
		return false;
	}
	if (bCollectFailed)
	{
		FinishWithoutWorker(*Record, ECustomNavBuildStatus::CollectionFailed, MoveTemp(Diagnostics));
		return false;
	}

	// --- Validation + freeze (game thread) ---
	FCustomNavBuildToken Token;
	Token.WorldLifetimeId = WorldLifetimeId;
	Token.Key = Key;
	Token.DesiredRevision = DesiredRevision;
	Token.RequestId = NextRequestId++;

	TSharedPtr<FCustomNavBuildSnapshot, ESPMode::ThreadSafe> Snapshot = MakeShared<FCustomNavBuildSnapshot, ESPMode::ThreadSafe>();
	const ECustomNavBuildStatus SnapshotStatus = CustomNavigation::BuildSnapshot(Token, Bounds, Settings, Collector.GetChunks(), *Snapshot, Diagnostics);
	if (SnapshotStatus != ECustomNavBuildStatus::Success)
	{
		FinishWithoutWorker(*Record, SnapshotStatus, MoveTemp(Diagnostics));
		return false;
	}

	// --- Dispatch ---
	TSharedRef<FCustomNavCancellation, ESPMode::ThreadSafe> Cancellation = MakeShared<FCustomNavCancellation, ESPMode::ThreadSafe>();
	Record->RunningRequestId = Token.RequestId;
	Record->RunningCancellation = Cancellation;
	Record->RunningCollectDiagnostics = MoveTemp(Diagnostics);
	ActiveRequestId = Token.RequestId;
	ActiveKey = Key;
	++LaunchedJobCount;

	UE_LOG(LogCustomNavigation, Verbose, TEXT("Dispatch %s: %d tris, %d verts"), *Token.ToString(), Snapshot->NumTriangles(), Snapshot->NumVertices());

	TSharedPtr<const FCustomNavBuildSnapshot, ESPMode::ThreadSafe> ConstSnapshot = MoveTemp(Snapshot);
	TSharedRef<FCustomNavBuildCompletionQueue, ESPMode::ThreadSafe> CompletionQueue = Completions.ToSharedRef();

	// The job owns only plain data: the snapshot, the cancellation flag and the completion queue.
	// It does not touch the world, the providers or any UObject, and does not call back into the game thread:
	// the subsystem picks the result up from the queue in its ticker.
	Executor->LaunchWorker([ConstSnapshot, Cancellation, CompletionQueue]() mutable
	{
		FCustomNavBuildResult BuildResult = FCustomNavRecastBuilder::Build(*ConstSnapshot, &Cancellation.Get());
		ConstSnapshot.Reset(); // release the input as soon as the build is done
		CompletionQueue->Results.Enqueue(MoveTemp(BuildResult));
	});
	EnsureTicker();
	return true;
}

void UCustomNavigationWorldSubsystem::OnBuildCompleted(FCustomNavBuildResult&& Result)
{
	check(IsInGameThread());
	const FCustomNavBuildToken& Token = Result.Token;

	if (Token.RequestId == ActiveRequestId)
	{
		ActiveRequestId = 0;
		ActiveKey = FCustomNavRegionKey();
	}

	if (bClosing || Token.WorldLifetimeId != WorldLifetimeId)
	{
		UE_LOG(LogCustomNavigation, Verbose, TEXT("Discard %s: world closing or replaced"), *Token.ToString());
		return;
	}

	FRegionRecord* Record = Regions.Find(Token.Key);
	if (!Record)
	{
		UE_LOG(LogCustomNavigation, Verbose, TEXT("Discard %s: region removed"), *Token.ToString());
		EnsureTicker();
		return;
	}

	const bool bIsRunningJob = Record->RunningRequestId == Token.RequestId;
	FCustomNavDiagnostics CollectDiagnostics;
	if (bIsRunningJob)
	{
		Record->RunningRequestId = 0;
		Record->RunningCancellation.Reset();
		CollectDiagnostics = MoveTemp(Record->RunningCollectDiagnostics);
		Record->RunningCollectDiagnostics = FCustomNavDiagnostics();
	}

	if (!bIsRunningJob || Token.DesiredRevision != Record->DesiredRevision || Result.Status == ECustomNavBuildStatus::Cancelled)
	{
		++Record->DiscardedCompletions;
		UE_LOG(LogCustomNavigation, Verbose, TEXT("Discard %s: obsolete (desired revision is %llu)"), *Token.ToString(), Record->DesiredRevision);
		if (Record->bPending)
		{
			EnqueuePending(Record->Key);
		}
		return;
	}

	CollectDiagnostics.Append(Result.Diagnostics);
	Record->LastOutcome = Result.Status;
	Record->LastOutcomeRevision = Token.DesiredRevision;
	Record->LastDiagnostics = MoveTemp(CollectDiagnostics);

	if (Result.Status == ECustomNavBuildStatus::Success && Result.Mesh.IsValid())
	{
		Record->Published = Result.Mesh;
		Record->PublishedRevision = Token.DesiredRevision;
		UE_LOG(LogCustomNavigation, Log, TEXT("Built region %s rev %llu: %d polys, %d detail tris in %.2f ms"),
		       *Token.Key.ToString(), Token.DesiredRevision, Result.Mesh->NumPolygons(), Result.Mesh->DetailTriangles.Num() / 3, Result.TotalBuildMs);
	}
	else
	{
		const FCustomNavDiagnostic* FirstError = Record->LastDiagnostics.FirstError();
		UE_LOG(LogCustomNavigation, Warning, TEXT("Build of region %s rev %llu finished with %s%s%s"),
		       *Token.Key.ToString(), Token.DesiredRevision, *UEnum::GetValueAsString(Result.Status),
		       FirstError ? TEXT(": ") : TEXT(""), FirstError ? *FirstError->ToString() : TEXT(""));
	}

	const FCustomNavRegionKey Key = Record->Key;
	const FCustomNavRegionStatus Status = MakeStatus(*Record);
	Record = nullptr; // listeners may modify the region map

	OnRegionBuildFinished.Broadcast(Key, Status);

	if (PendingQueue.Num() > 0)
	{
		EnsureTicker();
	}
}

void UCustomNavigationWorldSubsystem::FinishWithoutWorker(FRegionRecord& Record, ECustomNavBuildStatus Status, FCustomNavDiagnostics&& Diagnostics)
{
	Record.LastOutcome = Status;
	Record.LastOutcomeRevision = Record.DesiredRevision;
	Record.LastDiagnostics = MoveTemp(Diagnostics);

	const FCustomNavDiagnostic* FirstError = Record.LastDiagnostics.FirstError();
	UE_LOG(LogCustomNavigation, Warning, TEXT("Region %s rev %llu not built: %s%s%s"),
	       *Record.Key.ToString(), Record.DesiredRevision, *UEnum::GetValueAsString(Status),
	       FirstError ? TEXT(": ") : TEXT(""), FirstError ? *FirstError->ToString() : TEXT(""));

	const FCustomNavRegionKey Key = Record.Key;
	const FCustomNavRegionStatus RegionStatus = MakeStatus(Record);
	OnRegionBuildFinished.Broadcast(Key, RegionStatus);
}

FCustomNavRegionStatus UCustomNavigationWorldSubsystem::MakeStatus(const FRegionRecord& Record) const
{
	FCustomNavRegionStatus Status;
	Status.Key = Record.Key;
	Status.State = ComputeState(Record);
	Status.LastOutcome = Record.LastOutcome;
	Status.DesiredRevision = Record.DesiredRevision;
	Status.PublishedRevision = Record.Published.IsValid() ? Record.PublishedRevision : 0;
	Status.bPublishedIsStale = Record.Published.IsValid() && Record.PublishedRevision != Record.DesiredRevision;
	Status.LastDiagnostics = Record.LastDiagnostics;
	Status.DiscardedCompletions = Record.DiscardedCompletions;
	return Status;
}

ECustomNavRegionState UCustomNavigationWorldSubsystem::ComputeState(const FRegionRecord& Record) const
{
	if (Record.bPending)
	{
		return ECustomNavRegionState::Pending;
	}
	if (Record.RunningRequestId != 0)
	{
		return ECustomNavRegionState::Building;
	}
	if (Record.Published.IsValid() && Record.PublishedRevision == Record.DesiredRevision)
	{
		return ECustomNavRegionState::Ready;
	}
	if (Record.LastOutcomeRevision == Record.DesiredRevision && Record.LastOutcome != ECustomNavBuildStatus::None
		&& Record.LastOutcome != ECustomNavBuildStatus::Success)
	{
		return ECustomNavRegionState::Failed;
	}
	return ECustomNavRegionState::Idle;
}

void UCustomNavigationWorldSubsystem::SetBuildExecutorForTesting(TSharedPtr<FCustomNavBuildExecutor, ESPMode::ThreadSafe> InExecutor)
{
	check(IsInGameThread());
	Executor = InExecutor.IsValid() ? InExecutor : TSharedPtr<FCustomNavBuildExecutor, ESPMode::ThreadSafe>(FCustomNavBuildExecutor::MakeDefault());
}
