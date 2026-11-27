// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "CustomNavigationTypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCustomNavigation, Log, All);

/**
 * Area ids carried per triangle and per output polygon.
 * The values match Recast's area range: 0 = not walkable, 1..63 = walkable area ids.
 */
namespace CustomNavigation::Area
{
	/** Excludes a triangle from the navmesh (Recast RC_NULL_AREA). */
	inline constexpr uint8 Null = 0;
	/** Default walkable area (Recast RC_WALKABLE_AREA). */
	inline constexpr uint8 Walkable = 63;
	/** Highest valid area id. */
	inline constexpr uint8 Max = 63;
}

namespace CustomNavigation
{
	/** Largest agent radius in cells (ceil(AgentRadius / CellSize)). Recast stores twice the erosion distance in a byte. */
	inline constexpr int32 MaxErosionRadiusCells = 127;
}

/**
 * Settings for one region build. All distances are in Unreal units (centimeters).
 * Values are copied into the build snapshot, so changing them after a request does not affect a running build.
 * Defaults follow Unreal's default navmesh agent.
 */
USTRUCT(BlueprintType)
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavBuildSettings
{
	GENERATED_BODY()

	/** Horizontal voxel size [cm]. Smaller is more precise and slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxels", meta = (ClampMin = "1.0"))
	float CellSize = 19.f;

	/** Vertical voxel size [cm]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxels", meta = (ClampMin = "1.0"))
	float CellHeight = 10.f;

	/** Agent radius [cm]. The walkable area is eroded by this amount away from walls and region edges. At most 127 cells (ceil(AgentRadius / CellSize)). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Agent", meta = (ClampMin = "0.0"))
	float AgentRadius = 34.f;

	/** Agent height [cm]. Spaces with a lower ceiling are not walkable. Must be at least 3 * CellHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Agent", meta = (ClampMin = "0.0"))
	float AgentHeight = 144.f;

	/** Highest step the agent can climb [cm]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Agent", meta = (ClampMin = "0.0"))
	float AgentMaxClimb = 35.f;

	/** Steepest walkable slope [degrees], 0..89. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Agent", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float MaxSlopeDegrees = 44.f;

	/** Regions smaller than this many cells are removed (isolated islands). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regions", meta = (ClampMin = "0"))
	int32 MinRegionAreaCells = 64;

	/** Regions smaller than this many cells are merged into neighbours when possible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Regions", meta = (ClampMin = "0"))
	int32 MergeRegionAreaCells = 400;

	/** Longest allowed contour edge along the mesh border [cm]. 0 = no limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polygons", meta = (ClampMin = "0.0"))
	float MaxEdgeLength = 1200.f;

	/** How far simplified contours may deviate from the raw contour [cells]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polygons", meta = (ClampMin = "0.0"))
	float MaxSimplificationError = 1.3f;

	/** Maximum vertices per output polygon, 3..6. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polygons", meta = (ClampMin = "3", ClampMax = "6"))
	int32 MaxVertsPerPoly = 6;

	/** Build the detail (height) mesh. Without it the result has polygons only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	bool bBuildDetailMesh = true;

	/** Detail sampling distance [cm]. 0 disables sampling; otherwise must be >= 0.9 * CellSize. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0"))
	float DetailSampleDistance = 114.f;

	/** Maximum deviation of the detail mesh from the heightfield [cm]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0"))
	float DetailSampleMaxError = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filters")
	bool bFilterLowHangingObstacles = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filters")
	bool bFilterLedgeSpans = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filters")
	bool bFilterWalkableLowHeightSpans = true;

	/** Builds whose voxel grid (width * depth) exceeds this are rejected before any allocation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "1"))
	int64 MaxGridCells = 4096ll * 4096ll;

	/** Snapshot vertex limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "3"))
	int32 MaxInputVertices = 2000000;

	/** Snapshot triangle limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (ClampMin = "1"))
	int32 MaxInputTriangles = 2000000;

	bool Equals(const FCustomNavBuildSettings& Other) const;
};

/** Final outcome of one build attempt. */
UENUM(BlueprintType)
enum class ECustomNavBuildStatus : uint8
{
	/** No build finished yet. */
	None,
	Success,
	/** Providers returned no usable triangles. */
	NoGeometry,
	/** Geometry was valid but nothing walkable remained after filtering. */
	NoWalkableSurface,
	/** Snapshot validation failed (bad indices, NaN, bad settings, too large...). */
	InvalidInput,
	/** A provider failed or providers disagreed about a source. */
	CollectionFailed,
	/** A Recast stage failed. */
	BuildFailed,
	/** The build was cancelled cooperatively. */
	Cancelled,
};

/** Scheduling state of one region (independent from the last outcome). */
UENUM(BlueprintType)
enum class ECustomNavRegionState : uint8
{
	Idle,
	/** A build is wanted and waits for the worker slot. */
	Pending,
	Building,
	/** The published mesh matches the desired revision. */
	Ready,
	/** The last build of the desired revision failed. */
	Failed,
	/**
	 * A build is wanted, but the pending queue is full (FCustomNavSchedulingPolicy::MaxPendingRegions).
	 * The region keeps its place in a FIFO overflow list and moves to Pending when room frees; the change is not lost.
	 */
	Deferred,
};

UENUM(BlueprintType)
enum class ECustomNavDiagnosticSeverity : uint8
{
	Info,
	Warning,
	Error,
};

/** Machine-readable diagnostic codes. Tests assert on these. */
UENUM(BlueprintType)
enum class ECustomNavDiagnosticCode : uint8
{
	None,
	// Collection
	ProviderFailed,
	SourceConflict,
	DuplicateSourceSkipped,
	RegistryChangedDuringCollection,
	ChunkInvalid,
	// Validation
	InvalidBounds,
	InvalidSettings,
	NoGeometry,
	NonFiniteVertex,
	BadIndexCount,
	IndexOutOfRange,
	DegenerateTriangleDropped,
	BadAreaCount,
	InvalidAreaId,
	GridTooLarge,
	TooManyVertices,
	TooManyTriangles,
	// Build
	AllocationFailed,
	RecastStageFailed,
	RecastLog,
	NoWalkableSurface,
	Cancelled,
	OutputTooLarge,
	// Scheduling and dynamic correctness (appended to keep existing values stable)
	/** A provider cannot report its geometry changes, so the mesh may go out of date without notice. */
	ProviderChangesNotReported,
	/** The region kept changing for longer than MaxLatencySeconds; an older but finished build was published as stale. */
	PublishedUnderChurn,
};

/** One diagnostic entry. Plain data, safe to copy between threads. */
USTRUCT(BlueprintType)
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavDiagnostic
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	ECustomNavDiagnosticSeverity Severity = ECustomNavDiagnosticSeverity::Info;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	ECustomNavDiagnosticCode Code = ECustomNavDiagnosticCode::None;

	/** Pipeline stage, e.g. "Collect", "Validate", "Rasterize", "PolyMesh". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	FName Stage;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	FString Message;

	/** Source the diagnostic refers to, if any (FCustomNavSourceKey::ToString). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	FString Source;

	/** Triangle index inside the source chunk, or INDEX_NONE. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Navigation")
	int32 TriangleIndex = INDEX_NONE;

	FString ToString() const;
};

/**
 * Bounded diagnostic list. After MaxEntries entries further ones are counted but not stored,
 * so a broken mesh with a million bad triangles does not produce a million messages.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavDiagnostics
{
	static constexpr int32 MaxEntries = 64;

	TArray<FCustomNavDiagnostic> Entries;
	int32 DroppedCount = 0;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	void Add(ECustomNavDiagnosticSeverity Severity, ECustomNavDiagnosticCode Code, FName Stage, FString Message,
	         FString Source = FString(), int32 TriangleIndex = INDEX_NONE);
	void Error(ECustomNavDiagnosticCode Code, FName Stage, FString Message, FString Source = FString(), int32 TriangleIndex = INDEX_NONE)
	{
		Add(ECustomNavDiagnosticSeverity::Error, Code, Stage, MoveTemp(Message), MoveTemp(Source), TriangleIndex);
	}
	void Warning(ECustomNavDiagnosticCode Code, FName Stage, FString Message, FString Source = FString(), int32 TriangleIndex = INDEX_NONE)
	{
		Add(ECustomNavDiagnosticSeverity::Warning, Code, Stage, MoveTemp(Message), MoveTemp(Source), TriangleIndex);
	}
	void Append(const FCustomNavDiagnostics& Other);

	bool HasErrors() const { return ErrorCount > 0; }
	bool Contains(ECustomNavDiagnosticCode Code) const;
	/** First error entry, or nullptr. */
	const FCustomNavDiagnostic* FirstError() const;
	/** Multi-line text for logs. */
	FString ToString() const;
};

/** Identifies one region build: which volume and which agent profile. Not an actor id. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavRegionKey
{
	FName RegionId;
	FName AgentProfile;

	FCustomNavRegionKey() = default;
	FCustomNavRegionKey(FName InRegionId, FName InAgentProfile = NAME_None) : RegionId(InRegionId), AgentProfile(InAgentProfile) {}

	bool operator==(const FCustomNavRegionKey& Other) const { return RegionId == Other.RegionId && AgentProfile == Other.AgentProfile; }
	bool operator!=(const FCustomNavRegionKey& Other) const { return !(*this == Other); }
	friend uint32 GetTypeHash(const FCustomNavRegionKey& Key) { return HashCombine(GetTypeHash(Key.RegionId), GetTypeHash(Key.AgentProfile)); }
	FString ToString() const { return FString::Printf(TEXT("%s/%s"), *RegionId.ToString(), *AgentProfile.ToString()); }
};

/** A region build request passed to UCustomNavigationWorldSubsystem::RequestRegionBuild. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavRegionRequest
{
	FCustomNavRegionKey Key;
	/** World-space build volume [cm]. Geometry outside it is clipped by the voxelizer. */
	FBox WorldBounds = FBox(ForceInit);
	FCustomNavBuildSettings Settings;
};

/**
 * Immutable navmesh result in Unreal world space (centimeters, Z up).
 * Published as TSharedPtr<const FCustomNavRegionMesh, ESPMode::ThreadSafe>; a holder keeps it alive
 * even after the region is rebuilt or the world is destroyed. It holds no UObject or Recast pointers.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavRegionMesh
{
	FCustomNavRegionKey Key;
	/** Desired revision this mesh was built for. */
	uint64 Revision = 0;
	/** Build volume of the request. */
	FBox WorldBounds = FBox(ForceInit);
	/** Bounds of the produced polygons. */
	FBox MeshBounds = FBox(ForceInit);

	/** Polygon vertices, world space. Positions are quantized to the voxel grid (CellSize / CellHeight). */
	TArray<FVector> PolyVertices;
	/** Concatenated vertex indices of all polygons; polygon i uses PolyIndices[PolyOffsets[i] .. PolyOffsets[i+1]-1]. */
	TArray<int32> PolyIndices;
	/** Size NumPolygons() + 1. */
	TArray<int32> PolyOffsets;
	/** Area id per polygon (see CustomNavigation::Area). */
	TArray<uint8> PolyAreas;
	/** Recast region id per polygon (connected-area grouping, useful for debug colours). */
	TArray<uint16> PolyRegions;

	/** Detail mesh vertices, world space (exact heights). Empty when the detail mesh is disabled. */
	TArray<FVector> DetailVertices;
	/** Detail triangles, 3 indices into DetailVertices each, wound so the normal points up (+Z). */
	TArray<int32> DetailTriangles;
	/** Polygon index for each detail triangle. */
	TArray<int32> DetailTrianglePolys;

	/** Recast stage timings in milliseconds, keyed by stage name. */
	TMap<FName, double> StageTimingsMs;

	int32 NumPolygons() const { return FMath::Max(0, PolyOffsets.Num() - 1); }
	int32 NumPolygonVerts(int32 PolyIndex) const { return PolyOffsets[PolyIndex + 1] - PolyOffsets[PolyIndex]; }
	/** Sum of polygon areas projected on XY [cm^2]. */
	double ComputeProjectedArea() const;
	SIZE_T GetAllocatedSize() const;
};

using FCustomNavRegionMeshPtr = TSharedPtr<const FCustomNavRegionMesh, ESPMode::ThreadSafe>;

/** Status snapshot of one region, returned by value from the subsystem. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavRegionStatus
{
	FCustomNavRegionKey Key;
	ECustomNavRegionState State = ECustomNavRegionState::Idle;
	/** Outcome of the last finished (non-discarded) build. */
	ECustomNavBuildStatus LastOutcome = ECustomNavBuildStatus::None;
	uint64 DesiredRevision = 0;
	/** Revision of the published mesh, 0 if none. */
	uint64 PublishedRevision = 0;
	/** True if a published mesh exists but is older than DesiredRevision (geometry changed or last build failed). */
	bool bPublishedIsStale = false;
	/** Diagnostics of the last finished build attempt (collection, validation or Recast). */
	FCustomNavDiagnostics LastDiagnostics;
	/** Number of completions dropped because they were obsolete. */
	int32 DiscardedCompletions = 0;
	/**
	 * True if the published mesh was collected while a provider that cannot report changes was registered
	 * (ICustomNavGeometryProvider::ReportsGeometryChanges() == false). Such a mesh is not guaranteed to follow
	 * runtime geometry changes: treat it as correct only for static geometry.
	 */
	bool bDynamicCorrectnessUnsupported = false;
};

/**
 * When dirty regions are built. Defaults are tuning starting points, not measured targets.
 *
 * Without starvation: a region that keeps changing still gets a build at least every MaxLatencySeconds,
 * and once it has gone that long without any published update a running build is no longer cancelled by new
 * geometry changes; its result is published as stale (bPublishedIsStale) and the newest revision builds next.
 * See bPublishStaleUnderChurn for what disqualifies a build.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavSchedulingPolicy
{
	/** A dirty region waits until it has had no new change for this long, so a burst of changes produces one build. */
	double DebounceSeconds = 0.1;
	/** ...but never longer than this after its first unbuilt change. Also the starvation threshold (see above). */
	double MaxLatencySeconds = 1.0;
	/** Regions allowed in the pending queue at once; more become Deferred (FIFO). 0 = no limit. */
	int32 MaxPendingRegions = 64;
	/**
	 * Allows the starvation guard to publish an obsolete build as stale. Only geometry churn qualifies: a build is never
	 * published if providers were registered or unregistered, the region's bounds or settings changed, the region was
	 * re-created, or the world origin moved after it was dispatched. False = obsolete builds are always discarded.
	 */
	bool bPublishStaleUnderChurn = true;
};
