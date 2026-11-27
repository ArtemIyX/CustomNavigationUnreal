// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "Navigation/CustomNavigationTypes.h"

/**
 * Identity of one piece of source geometry.
 *
 * Two providers that can see the same real object MUST produce the same key for it, otherwise the
 * duplicate cannot be detected. Use a shared Domain per kind of source (e.g. "StaticMeshComponent")
 * and a stable ObjectId inside that domain (e.g. a hash of the component path); PartId separates
 * non-overlapping parts of one object.
 */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavSourceKey
{
	FName Domain;
	uint64 ObjectId = 0;
	uint32 PartId = 0;

	FCustomNavSourceKey() = default;
	FCustomNavSourceKey(FName InDomain, uint64 InObjectId, uint32 InPartId = 0) : Domain(InDomain), ObjectId(InObjectId), PartId(InPartId) {}

	bool operator==(const FCustomNavSourceKey& Other) const { return Domain == Other.Domain && ObjectId == Other.ObjectId && PartId == Other.PartId; }
	friend uint32 GetTypeHash(const FCustomNavSourceKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.Domain), GetTypeHash(Key.ObjectId)), GetTypeHash(Key.PartId));
	}
	FString ToString() const { return FString::Printf(TEXT("%s:%llu:%u"), *Domain.ToString(), ObjectId, PartId); }
};

/** What the coordinator asks providers for. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavGeometryRequest
{
	/** World-space volume of the build [cm]. Return every source that intersects it; chunks may extend beyond it. */
	FBox WorldBounds = FBox(ForceInit);
	FName AgentProfile;
};

/** One copied piece of geometry, owned by the collector. World space, centimeters. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavGeometryChunk
{
	FCustomNavSourceKey Key;
	/** Changes whenever this source's geometry changes. Equal key + equal revision means equal payload. */
	uint64 Revision = 0;
	TArray<FVector> Vertices;
	/** 3 per triangle, zero-based into Vertices. */
	TArray<int32> Indices;
	/** Empty = every triangle is a walkable candidate; otherwise one CustomNavigation::Area id per triangle. */
	TArray<uint8> TriangleAreas;
	/** Priority of the provider that produced the chunk (used to resolve key conflicts). */
	int32 ProviderPriority = 0;
	/** Debug name of the provider that produced the chunk. */
	FString ProviderName;
};

/**
 * Receives geometry from providers. All data is copied in; providers keep ownership of their buffers.
 *
 * Duplicate handling (by FCustomNavSourceKey):
 *  - same key, same revision and same payload  -> kept once (Info diagnostic);
 *  - same key, different payload, different provider priority -> higher priority wins (Warning);
 *  - same key, different payload, same priority -> conflict Error, collection fails.
 *
 * Triangle winding contract: triangles are given in Unreal world space and a walkable upward face
 * has a +Z normal by the right-hand rule, i.e. FVector::CrossProduct(B - A, C - A).Z > 0.
 */
class CUSTOMNAVIGATIONUNREAL_API FCustomNavGeometryCollector
{
public:
	explicit FCustomNavGeometryCollector(FCustomNavDiagnostics& InDiagnostics) : Diagnostics(InDiagnostics) {}

	/**
	 * Copies one source chunk. Returns false and records an error for malformed chunks
	 * (index count not a multiple of 3, area count mismatch) or an unresolvable key conflict.
	 */
	bool AddChunk(const FCustomNavSourceKey& Key, uint64 Revision,
	              TConstArrayView<FVector> Vertices, TConstArrayView<int32> Indices,
	              TConstArrayView<uint8> TriangleAreas = TConstArrayView<uint8>());

	/** Convenience: axis-aligned box (12 triangles, outward winding, top face walkable). */
	bool AddBox(const FCustomNavSourceKey& Key, uint64 Revision, const FBox& Box, uint8 Area = CustomNavigation::Area::Walkable);

	/** Convenience: horizontal rectangle at Center.Z facing up. */
	bool AddPlane(const FCustomNavSourceKey& Key, uint64 Revision, const FVector& Center, const FVector2D& HalfExtent, uint8 Area = CustomNavigation::Area::Walkable);

	const TArray<FCustomNavGeometryChunk>& GetChunks() const { return Chunks; }
	TArray<FCustomNavGeometryChunk>& GetChunksMutable() { return Chunks; }
	bool HasConflict() const { return bHasConflict; }

	/** Set by the coordinator before each provider call. */
	void SetCurrentProvider(const FString& Name, int32 Priority) { CurrentProviderName = Name; CurrentProviderPriority = Priority; }

private:
	FCustomNavDiagnostics& Diagnostics;
	TArray<FCustomNavGeometryChunk> Chunks;
	TMap<FCustomNavSourceKey, int32> ChunkIndexByKey;
	FString CurrentProviderName;
	int32 CurrentProviderPriority = 0;
	bool bHasConflict = false;
};

enum class ECustomNavCollectStatus : uint8
{
	Success,
	/** The provider could not collect geometry it is responsible for. The build fails instead of silently missing geometry. */
	Failed,
};

/**
 * Source of navigation geometry.
 *
 * Threading: CollectGeometry is called on the GAME THREAD by UCustomNavigationWorldSubsystem, synchronously,
 * once per build. It may read the world, actors and components. It must copy everything into the collector
 * and never hand out pointers to live data. Providers are never called from the worker thread.
 *
 * Change notification: when a provider's geometry changes it must call
 * UCustomNavigationWorldSubsystem::InvalidateGeometry with the old and new bounds (or InvalidateAll).
 * Without that the coordinator cannot know a published mesh is out of date. This is a correctness contract:
 * a provider that cannot keep it must say so through ReportsGeometryChanges().
 */
class CUSTOMNAVIGATIONUNREAL_API ICustomNavGeometryProvider
{
public:
	virtual ~ICustomNavGeometryProvider() = default;

	virtual ECustomNavCollectStatus CollectGeometry(const FCustomNavGeometryRequest& Request,
	                                                FCustomNavGeometryCollector& Collector,
	                                                FCustomNavDiagnostics& Diagnostics) = 0;

	virtual FString GetDebugName() const { return TEXT("GeometryProvider"); }

	/**
	 * True only if every change of this provider's geometry is reported through InvalidateGeometry / InvalidateAll.
	 * Defaults to false: the coordinator then still builds, but adds a ProviderChangesNotReported warning and flags
	 * the result bDynamicCorrectnessUnsupported instead of silently presenting possibly stale data as current.
	 */
	virtual bool ReportsGeometryChanges() const { return false; }
};

/** Opaque registration id returned by RegisterGeometryProvider. */
struct CUSTOMNAVIGATIONUNREAL_API FCustomNavGeometryProviderHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
	void Reset() { Id = 0; }
	bool operator==(const FCustomNavGeometryProviderHandle& Other) const { return Id == Other.Id; }
	friend uint32 GetTypeHash(const FCustomNavGeometryProviderHandle& Handle) { return GetTypeHash(Handle.Id); }
};
