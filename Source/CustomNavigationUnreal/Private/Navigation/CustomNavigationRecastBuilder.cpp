// Created by Stalker7274

#include "Navigation/Internal/CustomNavigationRecastBuilder.h"
#include "HAL/PlatformTime.h"
#include "Algo/Reverse.h"

THIRD_PARTY_INCLUDES_START
#include "Recast/Recast.h"
THIRD_PARTY_INCLUDES_END

namespace CustomNavigationPrivate
{
	static const FName StageHeightfield(TEXT("Heightfield"));
	static const FName StageMarkAreas(TEXT("MarkAreas"));
	static const FName StageRasterize(TEXT("Rasterize"));
	static const FName StageFilter(TEXT("Filter"));
	static const FName StageCompact(TEXT("CompactHeightfield"));
	static const FName StageErode(TEXT("Erode"));
	static const FName StageDistanceField(TEXT("DistanceField"));
	static const FName StageRegions(TEXT("Regions"));
	static const FName StageContours(TEXT("Contours"));
	static const FName StagePolyMesh(TEXT("PolyMesh"));
	static const FName StageDetailMesh(TEXT("DetailMesh"));
	static const FName StageConvert(TEXT("Convert"));

	/** RAII owner of one Recast object, freed with its matching rcFree* function. */
	template <typename T, void (*FreeFn)(T*)>
	struct TRecastPtr
	{
		T* Ptr = nullptr;
		explicit TRecastPtr(T* InPtr) : Ptr(InPtr) {}
		~TRecastPtr() { if (Ptr) { FreeFn(Ptr); } }
		TRecastPtr(const TRecastPtr&) = delete;
		TRecastPtr& operator=(const TRecastPtr&) = delete;
		T* operator->() const { return Ptr; }
		T& operator*() const { return *Ptr; }
		explicit operator bool() const { return Ptr != nullptr; }
		void Reset() { if (Ptr) { FreeFn(Ptr); Ptr = nullptr; } }
	};

	using FHeightfieldPtr = TRecastPtr<rcHeightfield, &rcFreeHeightField>;
	using FCompactPtr = TRecastPtr<rcCompactHeightfield, &rcFreeCompactHeightfield>;
	using FContourSetPtr = TRecastPtr<rcContourSet, &rcFreeContourSet>;
	using FPolyMeshPtr = TRecastPtr<rcPolyMesh, &rcFreePolyMesh>;
	using FDetailMeshPtr = TRecastPtr<rcPolyMeshDetail, &rcFreePolyMeshDetail>;

	/**
	 * Per-build Recast context. Routes Recast warnings/errors into the build's diagnostics.
	 * Lives on the worker stack; never shared, so no locking is needed. Recast's global log/alloc hooks are untouched.
	 */
	class FBuildContext final : public rcContext
	{
	public:
		FBuildContext(FCustomNavDiagnostics& InDiagnostics) : rcContext(true), Diagnostics(InDiagnostics)
		{
			enableTimer(false); // stage timings are measured by the builder itself
		}

		FName CurrentStage;

	protected:
		virtual void doLog(const rcLogCategory Category, const char* Msg, const int Len) override
		{
			if (Category == RC_LOG_PROGRESS || Msg == nullptr)
			{
				return;
			}
			FString Text = FString::ConstructFromPtrSize(Msg, FMath::Max(0, Len));
			Diagnostics.Add(Category == RC_LOG_ERROR ? ECustomNavDiagnosticSeverity::Error : ECustomNavDiagnosticSeverity::Warning,
			                ECustomNavDiagnosticCode::RecastLog, CurrentStage, MoveTemp(Text));
		}

		virtual void doResetLog() override {}

	private:
		FCustomNavDiagnostics& Diagnostics;
	};

	/** Measures one stage into Mesh timings and sets the context's stage for log messages. */
	struct FStageScope
	{
		FStageScope(TMap<FName, double>& InTimings, FBuildContext& InContext, FName InStage)
			: Timings(InTimings), Stage(InStage), Start(FPlatformTime::Seconds())
		{
			InContext.CurrentStage = InStage;
		}
		~FStageScope()
		{
			Timings.FindOrAdd(Stage) += (FPlatformTime::Seconds() - Start) * 1000.0;
		}
		TMap<FName, double>& Timings;
		FName Stage;
		double Start;
	};

	static double SignedAreaXY(const TArray<FVector>& Verts, const int32* Indices, int32 Count)
	{
		double Twice = 0.0;
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector& A = Verts[Indices[i]];
			const FVector& B = Verts[Indices[(i + 1) % Count]];
			Twice += A.X * B.Y - B.X * A.Y;
		}
		return Twice * 0.5;
	}
}

FCustomNavBuildResult FCustomNavRecastBuilder::Build(const FCustomNavBuildSnapshot& Snapshot, const FCustomNavCancellation* Cancellation)
{
	using namespace CustomNavigationPrivate;

	const double BuildStart = FPlatformTime::Seconds();
	FCustomNavBuildResult Result;
	Result.Token = Snapshot.Token;
	FCustomNavDiagnostics& Diag = Result.Diagnostics;

	TMap<FName, double> Timings;
	FBuildContext Ctx(Diag);

	auto Finish = [&](ECustomNavBuildStatus Status) -> FCustomNavBuildResult
	{
		Result.Status = Status;
		Result.TotalBuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
		if (Status != ECustomNavBuildStatus::Success)
		{
			Result.Mesh.Reset();
		}
		return MoveTemp(Result);
	};
	auto IsCancelled = [&]() -> bool
	{
		if (Cancellation && Cancellation->IsCancelled())
		{
			Diag.Add(ECustomNavDiagnosticSeverity::Info, ECustomNavDiagnosticCode::Cancelled, Ctx.CurrentStage, TEXT("Build cancelled"));
			return true;
		}
		return false;
	};
	auto StageFailed = [&](FName Stage, const TCHAR* What) -> FCustomNavBuildResult
	{
		Diag.Error(ECustomNavDiagnosticCode::RecastStageFailed, Stage, What);
		return Finish(ECustomNavBuildStatus::BuildFailed);
	};
	auto NoWalkable = [&](FName Stage, const TCHAR* What) -> FCustomNavBuildResult
	{
		Diag.Warning(ECustomNavDiagnosticCode::NoWalkableSurface, Stage, What);
		return Finish(ECustomNavBuildStatus::NoWalkableSurface);
	};

	// Defensive: the snapshot should already be valid, but the worker must never crash on bad data.
	const FCustomNavBuildSettings& S = Snapshot.Settings;
	if (Snapshot.NumTriangles() <= 0 || Snapshot.Vertices.Num() % 3 != 0 || Snapshot.TriangleAreas.Num() != Snapshot.NumTriangles()
		|| !(S.CellSize > 0.f) || !(S.CellHeight > 0.f))
	{
		Diag.Error(ECustomNavDiagnosticCode::ChunkInvalid, StageHeightfield, TEXT("Snapshot is malformed (was it produced by BuildSnapshot?)"));
		return Finish(ECustomNavBuildStatus::InvalidInput);
	}
	if (IsCancelled())
	{
		return Finish(ECustomNavBuildStatus::Cancelled);
	}

	// Unit conversion to voxels (same rounding as Unreal's and Recast's samples).
	const float Cs = S.CellSize;
	const float Ch = S.CellHeight;
	const int WalkableHeight = FMath::CeilToInt(S.AgentHeight / Ch);
	const int WalkableClimb = FMath::FloorToInt(S.AgentMaxClimb / Ch);
	const int WalkableRadius = FMath::CeilToInt(S.AgentRadius / Cs);
	const int MaxEdgeLen = FMath::FloorToInt(S.MaxEdgeLength / Cs);

	const float BMin[3] = {Snapshot.RecastBoundsMin.X, Snapshot.RecastBoundsMin.Y, Snapshot.RecastBoundsMin.Z};
	const float BMax[3] = {Snapshot.RecastBoundsMax.X, Snapshot.RecastBoundsMax.Y, Snapshot.RecastBoundsMax.Z};
	int GridW = 0, GridH = 0;
	rcCalcGridSize(BMin, BMax, Cs, &GridW, &GridH);
	if (GridW <= 0 || GridH <= 0 || static_cast<int64>(GridW) * GridH > S.MaxGridCells)
	{
		Diag.Error(ECustomNavDiagnosticCode::GridTooLarge, StageHeightfield, FString::Printf(TEXT("Grid %d x %d is empty or too large"), GridW, GridH));
		return Finish(ECustomNavBuildStatus::InvalidInput);
	}

	// 1. Heightfield
	FHeightfieldPtr Solid(rcAllocHeightfield());
	{
		FStageScope Scope(Timings, Ctx, StageHeightfield);
		if (!Solid)
		{
			Diag.Error(ECustomNavDiagnosticCode::AllocationFailed, StageHeightfield, TEXT("rcAllocHeightfield failed"));
			return Finish(ECustomNavBuildStatus::BuildFailed);
		}
		if (!rcCreateHeightfield(&Ctx, *Solid, GridW, GridH, BMin, BMax, Cs, Ch))
		{
			return StageFailed(StageHeightfield, TEXT("rcCreateHeightfield failed"));
		}
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 2. Areas: keep provider area ids, null out triangles steeper than the slope limit.
	TArray<uint8> Areas = Snapshot.TriangleAreas;
	{
		FStageScope Scope(Timings, Ctx, StageMarkAreas);
		rcClearUnwalkableTriangles(&Ctx, S.MaxSlopeDegrees, Snapshot.Vertices.GetData(), Snapshot.NumVertices(),
		                           Snapshot.Indices.GetData(), Snapshot.NumTriangles(), Areas.GetData());
	}

	// 3. Rasterize. Null-area triangles still rasterize and act as obstacles.
	{
		FStageScope Scope(Timings, Ctx, StageRasterize);
		if (!rcRasterizeTriangles(&Ctx, Snapshot.Vertices.GetData(), Snapshot.NumVertices(), Snapshot.Indices.GetData(), Areas.GetData(),
		                          Snapshot.NumTriangles(), *Solid, WalkableClimb))
		{
			return StageFailed(StageRasterize, TEXT("rcRasterizeTriangles failed"));
		}
	}
	Areas.Empty();
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 4. Filters
	{
		FStageScope Scope(Timings, Ctx, StageFilter);
		if (S.bFilterLowHangingObstacles) rcFilterLowHangingWalkableObstacles(&Ctx, WalkableClimb, *Solid);
		if (S.bFilterLedgeSpans) rcFilterLedgeSpans(&Ctx, WalkableHeight, WalkableClimb, *Solid);
		if (S.bFilterWalkableLowHeightSpans) rcFilterWalkableLowHeightSpans(&Ctx, WalkableHeight, *Solid);
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 5. Compact heightfield (only walkable spans survive)
	FCompactPtr Chf(rcAllocCompactHeightfield());
	{
		FStageScope Scope(Timings, Ctx, StageCompact);
		if (!Chf)
		{
			Diag.Error(ECustomNavDiagnosticCode::AllocationFailed, StageCompact, TEXT("rcAllocCompactHeightfield failed"));
			return Finish(ECustomNavBuildStatus::BuildFailed);
		}
		if (!rcBuildCompactHeightfield(&Ctx, WalkableHeight, WalkableClimb, *Solid, *Chf))
		{
			return StageFailed(StageCompact, TEXT("rcBuildCompactHeightfield failed"));
		}
	}
	Solid.Reset();
	if (Chf->spanCount == 0)
	{
		return NoWalkable(StageCompact, TEXT("No walkable spans (no upward-facing surfaces within the slope limit, or the agent does not fit)"));
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 6. Erode by agent radius
	{
		FStageScope Scope(Timings, Ctx, StageErode);
		if (WalkableRadius > 0 && !rcErodeWalkableArea(&Ctx, WalkableRadius, *Chf))
		{
			return StageFailed(StageErode, TEXT("rcErodeWalkableArea failed"));
		}
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 7. Distance field + watershed regions
	{
		FStageScope Scope(Timings, Ctx, StageDistanceField);
		if (!rcBuildDistanceField(&Ctx, *Chf))
		{
			return StageFailed(StageDistanceField, TEXT("rcBuildDistanceField failed"));
		}
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);
	{
		FStageScope Scope(Timings, Ctx, StageRegions);
		if (!rcBuildRegions(&Ctx, *Chf, 0, S.MinRegionAreaCells, S.MergeRegionAreaCells))
		{
			return StageFailed(StageRegions, TEXT("rcBuildRegions failed"));
		}
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 8. Contours
	FContourSetPtr Cset(rcAllocContourSet());
	{
		FStageScope Scope(Timings, Ctx, StageContours);
		if (!Cset)
		{
			Diag.Error(ECustomNavDiagnosticCode::AllocationFailed, StageContours, TEXT("rcAllocContourSet failed"));
			return Finish(ECustomNavBuildStatus::BuildFailed);
		}
		if (!rcBuildContours(&Ctx, *Chf, S.MaxSimplificationError, MaxEdgeLen, *Cset))
		{
			return StageFailed(StageContours, TEXT("rcBuildContours failed"));
		}
	}
	if (Cset->nconts == 0)
	{
		return NoWalkable(StageContours, TEXT("No contours: walkable area vanished after erosion or region filtering (MinRegionAreaCells / AgentRadius too large?)"));
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 9. Polygon mesh
	FPolyMeshPtr PMesh(rcAllocPolyMesh());
	{
		FStageScope Scope(Timings, Ctx, StagePolyMesh);
		if (!PMesh)
		{
			Diag.Error(ECustomNavDiagnosticCode::AllocationFailed, StagePolyMesh, TEXT("rcAllocPolyMesh failed"));
			return Finish(ECustomNavBuildStatus::BuildFailed);
		}
		if (!rcBuildPolyMesh(&Ctx, *Cset, S.MaxVertsPerPoly, *PMesh))
		{
			return StageFailed(StagePolyMesh, TEXT("rcBuildPolyMesh failed"));
		}
	}
	Cset.Reset();
	if (PMesh->npolys == 0)
	{
		return NoWalkable(StagePolyMesh, TEXT("Poly mesh is empty"));
	}
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 10. Detail mesh
	FDetailMeshPtr DMesh(nullptr);
	if (S.bBuildDetailMesh)
	{
		FStageScope Scope(Timings, Ctx, StageDetailMesh);
		DMesh.Ptr = rcAllocPolyMeshDetail();
		if (!DMesh)
		{
			Diag.Error(ECustomNavDiagnosticCode::AllocationFailed, StageDetailMesh, TEXT("rcAllocPolyMeshDetail failed"));
			return Finish(ECustomNavBuildStatus::BuildFailed);
		}
		if (!rcBuildPolyMeshDetail(&Ctx, *PMesh, *Chf, S.DetailSampleDistance, S.DetailSampleMaxError, *DMesh))
		{
			return StageFailed(StageDetailMesh, TEXT("rcBuildPolyMeshDetail failed"));
		}
	}
	Chf.Reset();
	if (IsCancelled()) return Finish(ECustomNavBuildStatus::Cancelled);

	// 11. Copy into the engine-side immutable mesh (world space).
	TSharedPtr<FCustomNavRegionMesh, ESPMode::ThreadSafe> Mesh = MakeShared<FCustomNavRegionMesh, ESPMode::ThreadSafe>();
	{
		FStageScope Scope(Timings, Ctx, StageConvert);
		const rcPolyMesh& P = *PMesh;
		const FVector& Origin = Snapshot.WorldOrigin;

		Mesh->Key = Snapshot.Token.Key;
		Mesh->Revision = Snapshot.Token.DesiredRevision;
		Mesh->WorldBounds = Snapshot.WorldBounds;

		Mesh->PolyVertices.Reserve(P.nverts);
		for (int32 i = 0; i < P.nverts; ++i)
		{
			const unsigned short* V = &P.verts[i * 3];
			const FVector World = CustomNavigation::RecastToWorld(P.bmin[0] + V[0] * P.cs, P.bmin[1] + V[1] * P.ch, P.bmin[2] + V[2] * P.cs, Origin);
			Mesh->PolyVertices.Add(World);
			Mesh->MeshBounds += World;
		}

		Mesh->PolyOffsets.Reserve(P.npolys + 1);
		Mesh->PolyIndices.Reserve(P.npolys * P.nvp);
		Mesh->PolyAreas.Reserve(P.npolys);
		Mesh->PolyRegions.Reserve(P.npolys);
		Mesh->PolyOffsets.Add(0);
		for (int32 PolyIndex = 0; PolyIndex < P.npolys; ++PolyIndex)
		{
			const unsigned short* Poly = &P.polys[PolyIndex * 2 * P.nvp];
			const int32 Start = Mesh->PolyIndices.Num();
			for (int32 j = 0; j < P.nvp && Poly[j] != RC_MESH_NULL_IDX; ++j)
			{
				Mesh->PolyIndices.Add(Poly[j]);
			}
			const int32 Count = Mesh->PolyIndices.Num() - Start;
			// Normalize to counter-clockwise seen from +Z.
			if (Count >= 3 && SignedAreaXY(Mesh->PolyVertices, &Mesh->PolyIndices[Start], Count) < 0.0)
			{
				Algo::Reverse(&Mesh->PolyIndices[Start], Count);
			}
			Mesh->PolyOffsets.Add(Mesh->PolyIndices.Num());
			Mesh->PolyAreas.Add(P.areas[PolyIndex]);
			Mesh->PolyRegions.Add(P.regs[PolyIndex]);
		}

		if (DMesh)
		{
			const rcPolyMeshDetail& D = *DMesh;
			Mesh->DetailVertices.Reserve(D.nverts);
			for (int32 i = 0; i < D.nverts; ++i)
			{
				Mesh->DetailVertices.Add(CustomNavigation::RecastToWorld(D.verts[i * 3 + 0], D.verts[i * 3 + 1], D.verts[i * 3 + 2], Origin));
			}
			Mesh->DetailTriangles.Reserve(D.ntris * 3);
			Mesh->DetailTrianglePolys.Reserve(D.ntris);
			for (int32 SubMesh = 0; SubMesh < D.nmeshes; ++SubMesh)
			{
				const uint32 VertBase = D.meshes[SubMesh * 4 + 0];
				const uint32 TriBase = D.meshes[SubMesh * 4 + 2];
				const uint32 TriCount = D.meshes[SubMesh * 4 + 3];
				for (uint32 t = 0; t < TriCount; ++t)
				{
					const unsigned char* Tri = &D.tris[(TriBase + t) * 4];
					int32 A = static_cast<int32>(VertBase + Tri[0]);
					int32 B = static_cast<int32>(VertBase + Tri[1]);
					int32 C = static_cast<int32>(VertBase + Tri[2]);
					if (A >= D.nverts || B >= D.nverts || C >= D.nverts)
					{
						continue;
					}
					const FVector& VA = Mesh->DetailVertices[A];
					if (FVector::CrossProduct(Mesh->DetailVertices[B] - VA, Mesh->DetailVertices[C] - VA).Z < 0.0)
					{
						Swap(B, C);
					}
					Mesh->DetailTriangles.Add(A);
					Mesh->DetailTriangles.Add(B);
					Mesh->DetailTriangles.Add(C);
					Mesh->DetailTrianglePolys.Add(SubMesh);
				}
			}
		}
	}

	Mesh->StageTimingsMs = MoveTemp(Timings);
	Result.Mesh = MoveTemp(Mesh);
	return Finish(ECustomNavBuildStatus::Success);
}
