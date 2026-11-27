// Created by Stalker7274

#pragma once

/**
 * INTERNAL: exported only for automation tests. Not a stable API.
 * Contains no Recast types.
 */

#include "CoreMinimal.h"
#include "Navigation/CustomNavigationTypes.h"
#include "Navigation/CustomNavigationGeometryProvider.h"

/** Everything that must match for a completion to be published. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavBuildToken
{
	/** Unique per subsystem instance in the process (never reused). */
	uint64 WorldLifetimeId = 0;
	FCustomNavRegionKey Key;
	uint64 DesiredRevision = 0;
	/** Unique per dispatched job. */
	uint64 RequestId = 0;
	/** Unique per region record: a region removed and requested again under the same key gets a new id. */
	uint64 RegionInstanceId = 0;
	/** World-origin generation at dispatch; the mesh is in that frame. */
	uint64 OriginGeneration = 0;

	FString ToString() const;
};

/**
 * Frozen input of one Recast build. Owns all data by value, no UObjects, no pointers to live data.
 * Shared to the worker as TSharedRef<const FCustomNavBuildSnapshot, ESPMode::ThreadSafe>.
 *
 * Coordinates: "Recast local" space.
 *   1. Origin (double, UE world) = centre of WorldBounds is subtracted in double precision.
 *   2. Axes are swapped: Recast (x, y, z) = UE local (X, Z, Y). Recast Y is up.
 *   3. Because the swap mirrors the space, every triangle (A, B, C) is stored as (A, C, B) so a UE
 *      upward face (+Z normal, right-hand rule) becomes a Recast upward face (+Y normal).
 * Units stay centimeters.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavBuildSnapshot
{
	FCustomNavBuildToken Token;
	FCustomNavBuildSettings Settings;

	FVector WorldOrigin = FVector::ZeroVector;
	FBox WorldBounds = FBox(ForceInit);
	/** Build volume in Recast local space. */
	FVector3f RecastBoundsMin = FVector3f::ZeroVector;
	FVector3f RecastBoundsMax = FVector3f::ZeroVector;

	/** x,y,z triples in Recast local space. */
	TArray<float> Vertices;
	/** 3 per triangle, already re-wound for Recast. */
	TArray<int32> Indices;
	/** One area id per triangle (always filled; defaults to CustomNavigation::Area::Walkable). */
	TArray<uint8> TriangleAreas;

	/** Source keys and revisions that went into this snapshot (diagnostics only). */
	TArray<TPair<FCustomNavSourceKey, uint64>> Sources;

	int32 NumVertices() const { return Vertices.Num() / 3; }
	int32 NumTriangles() const { return Indices.Num() / 3; }
};

using FCustomNavBuildSnapshotRef = TSharedRef<const FCustomNavBuildSnapshot, ESPMode::ThreadSafe>;

namespace CustomNavigation
{
	/** UE world -> Recast local (see FCustomNavBuildSnapshot). */
	CUSTOMNAVIGATIONUNREAL_API FVector3f WorldToRecast(const FVector& World, const FVector& Origin);
	/** Recast local -> UE world. */
	CUSTOMNAVIGATIONUNREAL_API FVector RecastToWorld(const FVector3f& Recast, const FVector& Origin);
	CUSTOMNAVIGATIONUNREAL_API FVector RecastToWorld(float X, float Y, float Z, const FVector& Origin);

	/** Voxel grid size the build would use, or false if bounds/settings are unusable. */
	CUSTOMNAVIGATIONUNREAL_API bool ComputeGridSize(const FBox& WorldBounds, const FCustomNavBuildSettings& Settings, int64& OutWidth, int64& OutDepth, int64& OutHeightCells);

	/** Validates settings alone. Returns false and adds InvalidSettings errors. */
	CUSTOMNAVIGATIONUNREAL_API bool ValidateSettings(const FCustomNavBuildSettings& Settings, FCustomNavDiagnostics& Diagnostics);

	/**
	 * Validates collected chunks and freezes them into a snapshot (game thread).
	 *
	 * Rejects (returns InvalidInput): bad bounds or settings, non-finite vertices, index count not a multiple of 3,
	 * out-of-range indices, wrong area count, area id > 63, too many vertices/triangles, too large grid.
	 * Returns NoGeometry when nothing usable is left.
	 * Degenerate triangles (repeated index or ~zero area) are dropped with a Warning; Recast cannot use them anyway.
	 *
	 * @return ECustomNavBuildStatus::Success and a filled OutSnapshot, or the failure status.
	 */
	CUSTOMNAVIGATIONUNREAL_API ECustomNavBuildStatus BuildSnapshot(const FCustomNavBuildToken& Token,
	                                                               const FBox& WorldBounds,
	                                                               const FCustomNavBuildSettings& Settings,
	                                                               const TArray<FCustomNavGeometryChunk>& Chunks,
	                                                               FCustomNavBuildSnapshot& OutSnapshot,
	                                                               FCustomNavDiagnostics& Diagnostics);
}
