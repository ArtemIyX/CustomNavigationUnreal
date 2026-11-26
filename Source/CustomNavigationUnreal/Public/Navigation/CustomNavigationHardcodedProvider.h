// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "Navigation/CustomNavigationGeometryProvider.h"

/**
 * Provider with geometry defined in code: planes, boxes and raw triangles.
 * Intended for smoke tests, automation tests and the debug actor; it is never registered automatically.
 *
 * Every Add... or Clear call bumps the revision. The provider does not know which subsystem it is registered with,
 * so after changing shapes call UCustomNavigationWorldSubsystem::InvalidateGeometry / InvalidateAll yourself.
 */
class CUSTOMNAVIGATIONUNREAL_API FCustomNavHardcodedGeometryProvider : public ICustomNavGeometryProvider
{
public:
	/** @param InDomain Key domain for the shapes; give two providers the same domain to make their shapes collide on purpose. */
	explicit FCustomNavHardcodedGeometryProvider(FName InDomain = TEXT("Hardcoded"));

	/** Adds a horizontal, upward-facing rectangle. Returns its part id. */
	uint32 AddPlane(const FVector& Center, const FVector2D& HalfExtent, uint8 Area = CustomNavigation::Area::Walkable);
	/** Adds a closed axis-aligned box (an obstacle; its top is walkable if large enough). */
	uint32 AddBox(const FBox& Box, uint8 Area = CustomNavigation::Area::Walkable);
	/** Adds raw triangles exactly as given (no validation here; the snapshot validator checks them). */
	uint32 AddTriangles(TArray<FVector> Vertices, TArray<int32> Indices, TArray<uint8> TriangleAreas = TArray<uint8>());

	/** Default smoke scene: a floor plane with a few obstacle boxes, centred at Center. */
	void AddDefaultTestScene(const FVector& Center = FVector::ZeroVector, double HalfSize = 2000.0);

	void Clear();

	/** Makes the next CollectGeometry call fail (test hook). */
	void SetFailCollection(bool bInFail) { bFailCollection = bInFail; }

	uint64 GetRevision() const { return Revision; }
	int32 NumShapes() const { return Shapes.Num(); }

	virtual ECustomNavCollectStatus CollectGeometry(const FCustomNavGeometryRequest& Request,
	                                                FCustomNavGeometryCollector& Collector,
	                                                FCustomNavDiagnostics& Diagnostics) override;
	virtual FString GetDebugName() const override;

private:
	struct FShape
	{
		uint32 PartId = 0;
		TArray<FVector> Vertices;
		TArray<int32> Indices;
		TArray<uint8> Areas;
		FBox Bounds = FBox(ForceInit);
	};

	FName Domain;
	uint64 ObjectId = 0;
	TArray<FShape> Shapes;
	uint32 NextPartId = 1;
	uint64 Revision = 1;
	bool bFailCollection = false;
};
