// Created by Stalker7274

#include "Navigation/Debug/CustomNavigationDebugComponent.h"
#include "DebugRenderSceneProxy.h"
#include "DynamicMeshBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "SceneView.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(CustomNavigationDebugComponent)

namespace CustomNavigationPrivate
{
	/** Debug proxy whose only addition is view relevance: FDebugRenderSceneProxy itself is not relevant to any view. */
	class FCustomNavDebugSceneProxy final : public FDebugRenderSceneProxy
	{
	public:
		FCustomNavDebugSceneProxy(const UPrimitiveComponent* InComponent, bool bInAlwaysDraw)
			: FDebugRenderSceneProxy(InComponent), bAlwaysDraw(bInAlwaysDraw)
		{
			DrawType = SolidAndWireMeshes;
			ViewFlagName = TEXT("Navigation");
			ViewFlagIndex = uint32(FEngineShowFlags::FindIndexByName(*ViewFlagName));
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			const bool bVisible = IsShown(View) && (bAlwaysDraw || View->Family->EngineShowFlags.Navigation);
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = bVisible;
			Result.bDynamicRelevance = true;
			Result.bSeparateTranslucency = Result.bNormalTranslucency = bVisible;
			return Result;
		}

	private:
		bool bAlwaysDraw;
	};

	static FColor RegionColor(uint16 Region, uint8 Alpha)
	{
		// Stable pseudo-random hue per region; region 0 (none) is grey.
		if (Region == 0)
		{
			return FColor(128, 128, 128, Alpha);
		}
		const uint32 Hash = ::GetTypeHash(static_cast<uint32>(Region) * 2654435761u);
		const uint8 Hue = static_cast<uint8>(Hash & 0xFF);
		FColor Color = FLinearColor::MakeFromHSV8(Hue, 160, 230).ToFColor(true);
		Color.A = Alpha;
		return Color;
	}

	static void AddTwoSidedTriangle(FDebugRenderSceneProxy::FMesh& Out, const FVector& A, const FVector& B, const FVector& C, const FColor& Color)
	{
		const uint32 Base = Out.Vertices.Num();
		Out.Vertices.Emplace(FVector3f(A), FVector2f::ZeroVector, Color);
		Out.Vertices.Emplace(FVector3f(B), FVector2f::ZeroVector, Color);
		Out.Vertices.Emplace(FVector3f(C), FVector2f::ZeroVector, Color);
		Out.Indices.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 1});
		Out.Box += A;
		Out.Box += B;
		Out.Box += C;
	}
}

UCustomNavigationDebugComponent::UCustomNavigationDebugComponent()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	bHiddenInGame = false;
	bIsEditorOnly = false;
	SetCastShadow(false);
	bSelectable = false;
}

void UCustomNavigationDebugComponent::SetMesh(FCustomNavRegionMeshPtr InMesh)
{
	Mesh = MoveTemp(InMesh);
	RefreshDrawing();
}

void UCustomNavigationDebugComponent::RefreshDrawing()
{
	UpdateBounds();
	MarkRenderStateDirty();
}

FDebugRenderSceneProxy* UCustomNavigationDebugComponent::CreateDebugSceneProxy()
{
	using namespace CustomNavigationPrivate;

	FCustomNavDebugSceneProxy* Proxy = new FCustomNavDebugSceneProxy(this, bAlwaysDraw);
	const FCustomNavRegionMesh* Data = Mesh.Get();
	if (!Data || Data->NumPolygons() == 0)
	{
		return Proxy;
	}

	const FVector Offset(0.0, 0.0, DrawOffsetZ);
	const uint8 Alpha = static_cast<uint8>(FMath::Clamp(FillAlpha, 0, 255));

	if (bDrawFill)
	{
		// One FMesh per Recast region keeps the draw count low and gives each region its own colour.
		TMap<uint16, int32> MeshByRegion;
		auto MeshFor = [&](int32 PolyIndex) -> FDebugRenderSceneProxy::FMesh&
		{
			const uint16 Region = Data->PolyRegions.IsValidIndex(PolyIndex) ? Data->PolyRegions[PolyIndex] : 0;
			if (const int32* Existing = MeshByRegion.Find(Region))
			{
				return Proxy->Meshes[*Existing];
			}
			const int32 NewIndex = Proxy->Meshes.AddDefaulted();
			Proxy->Meshes[NewIndex].Color = RegionColor(Region, Alpha);
			MeshByRegion.Add(Region, NewIndex);
			return Proxy->Meshes[NewIndex];
		};

		const bool bUseDetail = bPreferDetailMesh && Data->DetailTriangles.Num() > 0;
		if (bUseDetail)
		{
			for (int32 Tri = 0; Tri < Data->DetailTrianglePolys.Num(); ++Tri)
			{
				FDebugRenderSceneProxy::FMesh& Out = MeshFor(Data->DetailTrianglePolys[Tri]);
				AddTwoSidedTriangle(Out,
				                    Data->DetailVertices[Data->DetailTriangles[Tri * 3 + 0]] + Offset,
				                    Data->DetailVertices[Data->DetailTriangles[Tri * 3 + 1]] + Offset,
				                    Data->DetailVertices[Data->DetailTriangles[Tri * 3 + 2]] + Offset,
				                    Out.Color);
			}
		}
		else
		{
			for (int32 PolyIndex = 0; PolyIndex < Data->NumPolygons(); ++PolyIndex)
			{
				FDebugRenderSceneProxy::FMesh& Out = MeshFor(PolyIndex);
				const int32 Start = Data->PolyOffsets[PolyIndex];
				const int32 Count = Data->NumPolygonVerts(PolyIndex);
				for (int32 i = 1; i + 1 < Count; ++i)
				{
					AddTwoSidedTriangle(Out,
					                    Data->PolyVertices[Data->PolyIndices[Start]] + Offset,
					                    Data->PolyVertices[Data->PolyIndices[Start + i]] + Offset,
					                    Data->PolyVertices[Data->PolyIndices[Start + i + 1]] + Offset,
					                    Out.Color);
				}
			}
		}
	}

	if (bDrawPolygonEdges)
	{
		Proxy->Lines.Reserve(Data->PolyIndices.Num());
		for (int32 PolyIndex = 0; PolyIndex < Data->NumPolygons(); ++PolyIndex)
		{
			const int32 Start = Data->PolyOffsets[PolyIndex];
			const int32 Count = Data->NumPolygonVerts(PolyIndex);
			for (int32 i = 0; i < Count; ++i)
			{
				const FVector A = Data->PolyVertices[Data->PolyIndices[Start + i]] + Offset * 1.5;
				const FVector B = Data->PolyVertices[Data->PolyIndices[Start + (i + 1) % Count]] + Offset * 1.5;
				Proxy->Lines.Emplace(A, B, EdgeColor, 1.5f);
			}
		}
	}

	return Proxy;
}

FBoxSphereBounds UCustomNavigationDebugComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The mesh is already in world space; ignore LocalToWorld.
	if (Mesh.IsValid() && Mesh->MeshBounds.IsValid)
	{
		return FBoxSphereBounds(Mesh->MeshBounds.ExpandBy(FMath::Max(10.f, DrawOffsetZ * 2.f)));
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(50.0), 50.0);
}
