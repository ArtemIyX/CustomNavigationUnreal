// Created by Stalker7274

#pragma once

#include "CoreMinimal.h"
#include "Debug/DebugDrawComponent.h"
#include "Navigation/CustomNavigationTypes.h"
#include "CustomNavigationDebugComponent.generated.h"

/**
 * Draws one published FCustomNavRegionMesh: translucent filled polygons (coloured per Recast region)
 * plus polygon edges. Purely visual, it does not build anything. Feed it with SetMesh().
 *
 * The mesh is in world space, so the component's own transform is ignored.
 * Visible in editor and game views. With bAlwaysDraw = false it follows the "Navigation" show flag (P in the editor).
 */
UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent), hidecategories = (Object, LOD, Lighting, Transform, Sockets, TextureStreaming))
class CUSTOMNAVIGATIONUNREAL_API UCustomNavigationDebugComponent : public UDebugDrawComponent
{
	GENERATED_BODY()

public:
	UCustomNavigationDebugComponent();

	/** Sets the mesh to draw (nullptr clears). The shared mesh is immutable, so it can be kept safely. */
	void SetMesh(FCustomNavRegionMeshPtr InMesh);
	const FCustomNavRegionMeshPtr& GetMesh() const { return Mesh; }

	/** Draw regardless of the Navigation show flag. Off = toggled with P in the editor (or "show Navigation" in game), like the engine navmesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug")
	bool bAlwaysDraw = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug")
	bool bDrawFill = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug")
	bool bDrawPolygonEdges = true;

	/** Use the detail mesh (exact heights) for the fill when it exists; otherwise fan-triangulate polygons. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug")
	bool bPreferDetailMesh = true;

	/** Raises the drawing above the surface to avoid z-fighting [cm]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug", meta = (ClampMin = "0.0"))
	float DrawOffsetZ = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug", meta = (ClampMin = "0", ClampMax = "255"))
	int32 FillAlpha = 110;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Custom Navigation Debug")
	FColor EdgeColor = FColor(10, 40, 70, 255);

	/** Rebuilds the render proxy after changing properties at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Custom Navigation Debug")
	void RefreshDrawing();

protected:
	virtual FDebugRenderSceneProxy* CreateDebugSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	FCustomNavRegionMeshPtr Mesh;
};
