# CustomNavigationUnreal

Builds a Recast navmesh for a world-space volume from geometry supplied by **providers**, asynchronously, without touching Unreal's own navigation system. The result is an immutable, Unreal-space mesh (polygons and a detail mesh) that you can draw or query.

Current scope: one region is built in one piece (no tiles), there is no Detour navmesh or path queries yet, and geometry comes only from registered providers. The level is not scanned.

| Module | Type | Contents |
|---|---|---|
| `CustomNavigationUnreal` | Runtime | Provider API, snapshot, Recast builder, `UCustomNavigationWorldSubsystem`, debug actor and component |
| `RecastThirdParty` | Runtime | Vendored Recast + Detour (see [its README](Source/RecastThirdParty/README.md) and the [API reference](Source/RecastThirdParty/RECAST_API.md)) |
| `NavigationTests` | Editor | Automation tests (`CustomNavigation.*`) |

## Quick start: see a navmesh

1. Place **Custom Navigation Debug Actor** (`ACustomNavigationDebugActor`) in any level.
2. In an editor world it builds when the level is loaded (or the actor is placed or streamed in) and again after every move or property change (`bAutoRebuildInEditor`). While you drag, the previous mesh stays visible until the new one is published. In PIE or game it builds on `BeginPlay`. From code or Blueprint: `BuildNavigation()` and `ClearNavigation()`.
3. With `bUseHardcodedTestScene` set, the actor registers its own provider: a floor with three tall blocks (300 cm) and a low platform (80 cm, above the climb limit), centred on the actor. The blocks themselves are not drawn: only the navmesh is. The coloured fill is one colour per Recast region. White lines are polygon edges.
4. `LastBuildSummary` (Details panel) shows the outcome, polygon count, warnings and the first error.

Drawing uses `UCustomNavigationDebugComponent` (a `UDebugDrawComponent`). Like the engine navmesh, it follows the **Navigation** show flag: press `P` in an editor viewport (in PIE or game, use the `show Navigation` console command). Set `bAlwaysDraw` on the component to ignore the flag.

## Concepts

- **Region**: one build volume plus one agent profile, identified by `FCustomNavRegionKey { RegionId, AgentProfile }`. It is not tied to an actor.
- **Provider** (`ICustomNavGeometryProvider`): code that copies geometry into a collector on request.
- **Snapshot**: an owned, immutable, validated copy of all collected geometry in Recast space. It is the worker's only input.
- **Published mesh** (`FCustomNavRegionMeshPtr` = `TSharedPtr<const FCustomNavRegionMesh, ThreadSafe>`): the result in Unreal world space. Anyone holding the pointer keeps it alive, even after a rebuild or after the world is destroyed.
- **Revision**: each region has a `DesiredRevision`, bumped by every geometry, settings or provider change. A result is published only if it was built for the current revision.

## Using the subsystem (game thread only)

```cpp
#include "Navigation/CustomNavigationWorldSubsystem.h"

UCustomNavigationWorldSubsystem* Nav = UCustomNavigationWorldSubsystem::Get(GetWorld()); // Game, PIE and Editor worlds

// 1. Register a provider (the subsystem holds a shared ref until you unregister it).
TSharedRef<FMyProvider> Provider = MakeShared<FMyProvider>(/*...*/);
FCustomNavGeometryProviderHandle Handle = Nav->RegisterGeometryProvider(Provider, /*Priority*/ 0);

// 2. Request a region. Returns the desired revision (0 = rejected, e.g. RegionId is None).
FCustomNavRegionRequest Request;
Request.Key = FCustomNavRegionKey(TEXT("Arena"), TEXT("Default"));
Request.WorldBounds = FBox(FVector(-5000, -5000, -200), FVector(5000, 5000, 800));
Request.Settings.AgentRadius = 40.f;           // FCustomNavBuildSettings, all distances in cm
const uint64 Revision = Nav->RequestRegionBuild(Request);

// 3. Get the result: subscribe, or poll.
Nav->OnRegionBuildFinished.AddLambda([Nav](const FCustomNavRegionKey& Key, const FCustomNavRegionStatus& Status)
{
	if (Status.LastOutcome == ECustomNavBuildStatus::Success)
	{
		FCustomNavRegionMeshPtr Mesh = Nav->GetPublishedRegion(Key);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("%s"), *Status.LastDiagnostics.ToString());
	}
});

// 4. Tell the subsystem when geometry changes (old AND new bounds), or when everything changes.
Nav->InvalidateGeometry(OldBounds);
Nav->InvalidateGeometry(NewBounds);
Nav->InvalidateAll();

// 5. Clean up.
Nav->RemoveRegion(Request.Key);
Nav->UnregisterGeometryProvider(Handle);   // also invalidates all regions
```

Other calls:
- `GetRegionStatus(Key)` returns `FCustomNavRegionStatus`: state, last outcome, revisions, `bPublishedIsStale`, `bDynamicCorrectnessUnsupported`, diagnostics and `DiscardedCompletions`.
- `GetPublishedRegion(Key, bAllowStale)`: by default returns nothing while the mesh is older than the desired revision. Pass `true` to keep drawing the last good mesh.
- `HasRegion`, `GetRegionKeys`, `IsBuildRunning`, `IsClosing`.
- `FlushPendingBuilds()` starts the next due pending build now instead of on the next ticker tick (debounce still applies).
- `SetSchedulingPolicy(FCustomNavSchedulingPolicy)`, `NumPendingRegions()`, `NumDeferredRegions()`: see Scheduling below.
- `ProcessCompletedBuilds(BudgetSeconds = -1)` applies finished builds now instead of on the next ticker tick. Normally not needed.

Rules for requests:
- A request identical to the last one (same bounds and `Settings.Equals`) that is pending, building or ready returns the existing revision and starts nothing.
- An identical request after a failure retries the build.
- A different request bumps the revision and cancels the running build of that region.

### Scheduling

`FCustomNavSchedulingPolicy` (per world, `SetSchedulingPolicy`):

| Field | Default | Meaning |
|---|---|---|
| `DebounceSeconds` | 0.1 | A dirty region waits until it has had no new change for this long. |
| `MaxLatencySeconds` | 1.0 | ...but starts no later than this after its first change, even if changes keep coming. |
| `MaxPendingRegions` | 64 | Size of the pending queue. Further dirty regions become `Deferred` (0 = no limit). |

Rules:
- Among due regions the oldest queued starts first. A region still inside its debounce window does not block others. Repeated changes keep a region's place in the queue.
- **No starvation.** A change normally cancels the running build of that region. Once the region's oldest unpublished change is `MaxLatencySeconds` old, the running build is no longer cancelled. If it finishes while newer changes are pending, its mesh is published **as stale** (`bPublishedIsStale`, warning `PublishedUnderChurn`) and the next build follows. So geometry that changes every frame still gets a fresh mesh about every `MaxLatencySeconds`, readable with `GetPublishedRegion(Key, true)`.
- **Deferred.** Overflow regions wait in FIFO order with state `Deferred` and move to the pending queue as soon as room frees. Each region is queued at most once, so memory is bounded by the region count.

### Region states

`ECustomNavRegionState`:

| State | Meaning |
|---|---|
| `Pending` | The region is dirty and waiting for the build slot (or for its debounce). |
| `Deferred` | The region is dirty, but the pending queue is full (`MaxPendingRegions`). It waits for room. |
| `Building` | Its job is running. |
| `Ready` | The published mesh matches the desired revision. |
| `Failed` | The last attempt for the desired revision did not succeed. |
| `Idle` | Anything else. |

`ECustomNavBuildStatus` (last outcome):

| Outcome | Meaning |
|---|---|
| `Success` | A mesh was published. |
| `NoGeometry` | Providers gave no usable triangles. |
| `NoWalkableSurface` | The geometry was valid, but nothing walkable is left. |
| `InvalidInput` | Snapshot validation failed. |
| `CollectionFailed` | A provider failed, or providers conflicted. |
| `BuildFailed` | A Recast stage or an allocation failed. |
| `Cancelled` | The build was cancelled. |

## Writing a provider

```cpp
#include "Navigation/CustomNavigationGeometryProvider.h"

class FMyProvider final : public ICustomNavGeometryProvider
{
public:
	virtual ECustomNavCollectStatus CollectGeometry(const FCustomNavGeometryRequest& Request,
	                                                FCustomNavGeometryCollector& Collector,
	                                                FCustomNavDiagnostics& Diagnostics) override
	{
		for (const FMySource& Source : Sources)
		{
			if (!Source.Bounds.Intersect(Request.WorldBounds)) { continue; }
			// Key must be identical in every provider that can see the same object.
			const FCustomNavSourceKey Key(TEXT("StaticMeshComponent"), Source.StableId, /*PartId*/ 0);
			Collector.AddChunk(Key, Source.Revision, Source.WorldVertices, Source.Indices /*, optional per-triangle areas */);
		}
		return ECustomNavCollectStatus::Success;   // Failed = "I could not provide what I own": the build fails visibly
	}
	virtual FString GetDebugName() const override { return TEXT("MyProvider"); }
};
```

Contract:
- `CollectGeometry` runs **on the game thread**, synchronously, once per build. It may read actors and components. Everything is **copied** into the collector, so no pointers to live data escape.
- **Units and space**: Unreal world space, centimeters, Z up.
- **Winding**: a walkable upward face has a +Z normal by the right-hand rule: `FVector::CrossProduct(B - A, C - A).Z > 0`. A face wound the other way is treated as facing down and is not walkable. It still blocks space as an obstacle.
- **Areas**: optional `TriangleAreas`, one per triangle. `CustomNavigation::Area::Null` (0) excludes a triangle from walking, but it still rasterizes as an obstacle. Values 1..63 are walkable area ids; the default is `Walkable` (63). Triangles steeper than `MaxSlopeDegrees` become Null automatically.
- **Keys and duplicates**: with two providers that see the same object, matching keys are resolved as follows:

  | Same key with... | Result |
  |---|---|
  | Same revision and payload | Kept once (Info). |
  | Different payload, different priority | The higher priority wins (Warning). |
  | Different payload, same priority | `SourceConflict` error, `CollectionFailed`. |

  Different keys are never deduplicated, so overlapping copies of the same object with different keys are both used.
- **Changes**: call `InvalidateGeometry(OldBounds)` and `InvalidateGeometry(NewBounds)`, or `InvalidateAll()`. Without this the subsystem never learns a mesh is out of date.
- **`ReportsGeometryChanges()`**: return `true` only if every change to your geometry is followed by such an invalidation. The default is `false`: the provider is still used, but every build that includes it gets the warning `ProviderChangesNotReported`, and its published mesh has `bDynamicCorrectnessUnsupported = true` (valid for static geometry only).
- A provider may call back into the subsystem from `CollectGeometry`, for example to invalidate. The subsystem re-checks its state afterwards. If the region's revision changed during collection, the collected data is dropped and collection runs again later.
- `FCustomNavGeometryCollector` helpers: `AddBox` (12 triangles, outward winding, walkable top) and `AddPlane` (an upward rectangle).

`FCustomNavHardcodedGeometryProvider` is a ready-made provider for tests and samples:
- Methods: `AddPlane`, `AddBox`, `AddTriangles`, `AddDefaultTestScene(Center, HalfSize)`, `Clear`, `SetFailCollection`, `SetReportsGeometryChanges` (default `true`).
- Each `Add*` returns a part id. Ids keep increasing after `Clear()`.
- After changing it while it is registered, call `InvalidateGeometry` or `InvalidateAll`.

## Threads and lifetime

| Step | Thread | Touches |
|---|---|---|
| Request, invalidate, register, query | Game | Subsystem state |
| Provider collection, snapshot build and validation | Game (inside the scheduler tick or `FlushPendingBuilds`) | Providers, world, then plain arrays |
| Recast build | Dedicated `CustomNavBuild` thread (`FRunnable`) | **Only** the snapshot, a cancellation flag and the world's completion queue. No UObjects, no subsystem, no providers. |
| Result handed over | Build thread | Pushes the result into the world's completion queue (lock-free `TQueue`). Nothing is posted to the game thread. |
| Completion: discard or publish, then broadcast `OnRegionBuildFinished` | Game (scheduler tick) | Subsystem state |

- **One build thread for all worlds.** `FCustomNavBuildThread` is created on the first build, runs jobs one after another in FIFO order (editor and PIE worlds share it), runs at below-normal priority and sleeps on an event while idle. Without multithreading support (`-nothreading`, some commandlets) jobs run inline.
- **One build runs at a time per world.** Other dirty regions wait in a bounded queue (see Scheduling). The scheduler is an `FTSTicker` that is armed only while there is pending work, a running job or an undrained result.
- **Results are polled, not called back.** Each tick, the subsystem first drains its completion queue, then starts the next pending region. Draining stops after `CustomNav.CompletionBudgetMs` (default 1 ms, `<= 0` = no limit) per world per tick, but at least one result is always applied, so the game thread never does more than that per frame and the queue always makes progress.
- **The worker owns its input.** The job holds a `TSharedPtr<const FCustomNavBuildSnapshot>` and releases it as soon as Recast finishes. Providers, actors and the world may change or die meanwhile.
- **Stale results are discarded.** A completion is dropped, and `DiscardedCompletions` is incremented, in any of these cases:
  - the world is closing, or the result belongs to another world lifetime;
  - the region was removed;
  - the request id does not match;
  - the revision is no longer desired;
  - the build was cancelled.

  Only a result for the current revision is published.
- **Cancellation is cooperative.** It is checked between Recast stages, never in the middle of one.
- **Teardown**: `Deinitialize` cancels jobs, stops the ticker, drops its reference to the completion queue and clears everything. A job that finishes later pushes into a queue only it still references, and the result is freed with the job, even if the subsystem was already garbage collected. Module shutdown stops the build thread and waits for the running job, because it executes this module's code; queued jobs are dropped unrun.
- The global Recast allocator and logging are not changed. Recast messages go to the build's diagnostics through a per-build `rcContext`.

## Coordinates (snapshot and Recast space)

Recast is Y-up and expects counter-clockwise triangles seen from above. The snapshot converts as follows:

```
Origin   = WorldBounds.GetCenter()                     (subtracted in double precision, then stored as float)
Recast   = (UE.X - O.X, UE.Z - O.Z, UE.Y - O.Y)        // x, y(up), z
UE       = (R.x + O.X, R.z + O.Y, R.y + O.Z)
Triangle (A, B, C) in UE  ->  (A, C, B) in Recast       // swapping axes mirrors, so the winding is flipped back
```

This keeps float precision local, so a region millions of cm from the world origin still builds correctly (there is a test for it). Output meshes are converted back to Unreal world space:
- Polygons are counter-clockwise seen from +Z.
- Detail triangles have a +Z normal.
- `DetailTrianglePolys[i]` is the polygon that owns detail triangle `i`.

Build settings in centimeters are converted to Recast voxel units:

| Setting | Recast value |
|---|---|
| `AgentHeight` | `walkableHeight = ceil(AgentHeight / CellHeight)` |
| `AgentMaxClimb` | `walkableClimb = floor(AgentMaxClimb / CellHeight)` |
| `AgentRadius` | `walkableRadius = ceil(AgentRadius / CellSize)` |
| `MaxEdgeLength` | `maxEdgeLen = floor(MaxEdgeLength / CellSize)` |

`MaxSimplificationError` is in cells. `DetailSampleDistance` and `DetailSampleMaxError` are in cm.

## Validation and diagnostics

Validation runs on the game thread before any worker is launched. Each failure yields `InvalidInput` with a specific code:

| Code | Cause |
|---|---|
| `InvalidSettings` | Cell size ≤ 0; agent height below 3 cells; climb ≥ height; slope outside 0..89; verts per polygon outside 3..6; detail sampling distance below 0.9 × cell size; and similar range checks. |
| `InvalidBounds` | The bounds are not set, or have zero size on some axis. |
| `GridTooLarge` | The XY cell count exceeds `MaxGridCells`, or the height exceeds 8191 cells. |
| `NonFiniteVertex` | A vertex is NaN or infinite. |
| `IndexOutOfRange` | An index is outside the vertex array. |
| `BadIndexCount` | The index count is not a multiple of 3. |
| `BadAreaCount` | The area array size does not match the triangle count. |
| `InvalidAreaId` | An area id is above 63. |
| `TooManyVertices`, `TooManyTriangles` | The input exceeds `MaxInputVertices` or `MaxInputTriangles`. |

Not every problem fails validation:
- Degenerate triangles are dropped with a `DegenerateTriangleDropped` warning.
- If nothing usable remains, the outcome is `NoGeometry`.

`FCustomNavDiagnostics` holds at most 64 entries. Further entries are only counted (`DroppedCount`). Each entry carries severity, code, stage, message, source key and triangle index. On the worker, Recast warnings and errors arrive as `RecastLog` entries, tagged with the stage. Build results also carry `StageTimingsMs`.

## Debug actor reference

`ACustomNavigationDebugActor` properties:

| Property | Meaning |
|---|---|
| `BuildVolume` | Root box. The request bounds are its axis-aligned scaled extent; rotation is ignored. |
| `RegionId` | Empty means the actor's name. |
| `AgentProfile` | Agent profile part of the region key. |
| `Settings` | `FCustomNavBuildSettings` for the request. |
| `bUseHardcodedTestScene`, `TestSceneHalfSize` | Built-in test provider; a half size of 0 means 90 % of the volume. |
| `bBuildOnBeginPlay`, `bAutoRebuildInEditor` | When builds happen automatically: on BeginPlay in game worlds; in editor worlds on level load, placement, move and property change. An unchanged scene and an identical request start no new build. |
| `LastBuildSummary` | Result of the last finished build. |

Each actor registers its test provider under its own domain (`DebugActor_<path>`), so several actors never conflict. `EndPlay` and `Destroyed` unregister the provider and remove the region.

`UCustomNavigationDebugComponent` can be used on its own: call `SetMesh(Mesh)`.
- Properties: `bDrawFill`, `bDrawPolygonEdges`, `bPreferDetailMesh`, `DrawOffsetZ`, `FillAlpha`, `EdgeColor`, `bAlwaysDraw`.
- Call `RefreshDrawing()` after changing them at runtime.

## Tests

Close the editor, build the editor target, then run:

```powershell
& "U:/Engines/UnrealEngine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<Project>.uproject" "-ExecCmds=Automation RunTests CustomNavigation;Quit" "-testexit=Automation Test Queue Empty" -unattended -nullrhi -nopause -abslog="<log file>"
```

| Test | Covers |
|---|---|
| `CustomNavigation.Build.Coordinates` | Axis swap, round trip, winding after the rewind |
| `CustomNavigation.Build.Validation` | Every validation code, degenerate handling, bounded diagnostics |
| `CustomNavigation.Build.Collector` | Duplicate, priority and conflict rules |
| `CustomNavigation.Build.Recast` | Plane area (erosion), CCW polygons, +Z detail mesh, far-from-origin build, downward, wall-only and null-area input, obstacle, default scene, cancellation, malformed snapshot |
| `CustomNavigation.Coordinator.Publish` | Pending, building and ready; request sharing; invalidation by bounds; stale access; mesh outliving the world |
| `CustomNavigation.Coordinator.StaleResult` | Result N discarded after a change, N+1 published, removed region not resurrected |
| `CustomNavigation.Coordinator.Teardown` | World destroyed during a build; the job runs after the subsystem is garbage collected |
| `CustomNavigation.Coordinator.Failures` | Invalid settings or geometry (no worker), provider failure and retry, duplicates and conflict, empty input |
| `CustomNavigation.Coordinator.Scheduling` | Debounce, max latency under constant changes, starvation guard (stale publish), pending limit with `Deferred`, provider without change reports |
| `CustomNavigation.Coordinator.AsyncSmoke` | Real path: the job runs on the `CustomNavBuild` thread and the ticker applies the result |
| `CustomNavigation.API.*`, `CustomNavigation.Workflow.*` | Raw Recast and Detour smoke tests |

Coordinator tests swap in a manual executor (`SetBuildExecutorForTesting`, `SetAutoScheduleForTesting(false)`), so the test decides exactly when jobs run (`FManualQueue::RunWorkers`) and when results are applied (`ProcessCompletedBuilds`). They also set a fake clock (`SetClockForTesting`) and zero debounce, so scheduling is deterministic.

## Limitations and known issues

- **Single region, single tile.** Large worlds need tiling (planned). `MaxGridCells` guards against huge single builds.
- **No Detour or queries yet.** The output is a polygon mesh for drawing and inspection. Path finding comes with the Detour step.
- **Geometry only from providers.** There is no automatic collection of static meshes, landscape or collision yet.
- **One build thread for the whole process.** Builds of all worlds (editor, PIE clients) run one after another on `CustomNavBuild`; there is no parallelism across cores yet.
- **One build per world at a time.** Under constant changes a region gets a stale mesh about every `MaxLatencySeconds`, not a current one.
- **Monolithic (Game target) builds are untested.** The vendored Recast and Detour symbols may clash with the engine's own `Navmesh` module when everything links into one binary. Editor (modular) builds work.
- Actor rotation is ignored for the debug actor's volume. Only axis-aligned bounds are supported.
- Cancellation granularity is one Recast stage.
