// Created by Stalker7274

#include "Navigation/CustomNavigationGeometryProvider.h"
#include "Navigation/CustomNavigationHardcodedProvider.h"

namespace CustomNavigationPrivate
{
	static const FName StageCollect(TEXT("Collect"));

	static bool PayloadEquals(const FCustomNavGeometryChunk& Chunk, TConstArrayView<FVector> Vertices, TConstArrayView<int32> Indices, TConstArrayView<uint8> Areas)
	{
		if (Chunk.Vertices.Num() != Vertices.Num() || Chunk.Indices.Num() != Indices.Num() || Chunk.TriangleAreas.Num() != Areas.Num())
		{
			return false;
		}
		// Bitwise comparison: a duplicate must be exactly the same data, NaNs included.
		return FMemory::Memcmp(Chunk.Vertices.GetData(), Vertices.GetData(), Vertices.Num() * sizeof(FVector)) == 0
			&& FMemory::Memcmp(Chunk.Indices.GetData(), Indices.GetData(), Indices.Num() * sizeof(int32)) == 0
			&& FMemory::Memcmp(Chunk.TriangleAreas.GetData(), Areas.GetData(), Areas.Num() * sizeof(uint8)) == 0;
	}

	/** Builds a closed box with outward-facing triangles (right-hand rule in UE space). */
	static void MakeBox(const FBox& Box, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
	{
		const FVector& Mn = Box.Min;
		const FVector& Mx = Box.Max;
		OutVertices = {
			FVector(Mn.X, Mn.Y, Mn.Z), FVector(Mx.X, Mn.Y, Mn.Z), FVector(Mx.X, Mx.Y, Mn.Z), FVector(Mn.X, Mx.Y, Mn.Z),
			FVector(Mn.X, Mn.Y, Mx.Z), FVector(Mx.X, Mn.Y, Mx.Z), FVector(Mx.X, Mx.Y, Mx.Z), FVector(Mn.X, Mx.Y, Mx.Z),
		};
		OutIndices = {
			4, 5, 6, 4, 6, 7, // top    (+Z)
			0, 2, 1, 0, 3, 2, // bottom (-Z)
			0, 1, 5, 0, 5, 4, // -Y
			2, 3, 7, 2, 7, 6, // +Y
			1, 2, 6, 1, 6, 5, // +X
			3, 0, 4, 3, 4, 7, // -X
		};
	}

	static void MakePlane(const FVector& Center, const FVector2D& HalfExtent, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
	{
		OutVertices = {
			FVector(Center.X - HalfExtent.X, Center.Y - HalfExtent.Y, Center.Z),
			FVector(Center.X + HalfExtent.X, Center.Y - HalfExtent.Y, Center.Z),
			FVector(Center.X + HalfExtent.X, Center.Y + HalfExtent.Y, Center.Z),
			FVector(Center.X - HalfExtent.X, Center.Y + HalfExtent.Y, Center.Z),
		};
		// Counter-clockwise seen from +Z => normal +Z.
		OutIndices = {0, 1, 2, 0, 2, 3};
	}
}

bool FCustomNavGeometryCollector::AddChunk(const FCustomNavSourceKey& Key, uint64 Revision,
                                           TConstArrayView<FVector> Vertices, TConstArrayView<int32> Indices,
                                           TConstArrayView<uint8> TriangleAreas)
{
	using namespace CustomNavigationPrivate;

	if (Indices.Num() % 3 != 0)
	{
		Diagnostics.Error(ECustomNavDiagnosticCode::BadIndexCount, StageCollect,
			FString::Printf(TEXT("Index count %d is not a multiple of 3 (provider %s)"), Indices.Num(), *CurrentProviderName), Key.ToString());
		return false;
	}
	if (TriangleAreas.Num() != 0 && TriangleAreas.Num() != Indices.Num() / 3)
	{
		Diagnostics.Error(ECustomNavDiagnosticCode::BadAreaCount, StageCollect,
			FString::Printf(TEXT("%d area ids for %d triangles (provider %s)"), TriangleAreas.Num(), Indices.Num() / 3, *CurrentProviderName), Key.ToString());
		return false;
	}

	if (const int32* ExistingIndex = ChunkIndexByKey.Find(Key))
	{
		FCustomNavGeometryChunk& Existing = Chunks[*ExistingIndex];
		if (Existing.Revision == Revision && PayloadEquals(Existing, Vertices, Indices, TriangleAreas))
		{
			Diagnostics.Add(ECustomNavDiagnosticSeverity::Info, ECustomNavDiagnosticCode::DuplicateSourceSkipped, StageCollect,
				FString::Printf(TEXT("Identical source from %s and %s collected once"), *Existing.ProviderName, *CurrentProviderName), Key.ToString());
			return true;
		}
		if (Existing.ProviderPriority != CurrentProviderPriority)
		{
			const bool bReplace = CurrentProviderPriority > Existing.ProviderPriority;
			Diagnostics.Warning(ECustomNavDiagnosticCode::DuplicateSourceSkipped, StageCollect,
				FString::Printf(TEXT("Conflicting source resolved by priority: kept %s (priority %d), ignored %s (priority %d)"),
					bReplace ? *CurrentProviderName : *Existing.ProviderName, FMath::Max(CurrentProviderPriority, Existing.ProviderPriority),
					bReplace ? *Existing.ProviderName : *CurrentProviderName, FMath::Min(CurrentProviderPriority, Existing.ProviderPriority)),
				Key.ToString());
			if (bReplace)
			{
				Existing.Revision = Revision;
				Existing.Vertices = TArray<FVector>(Vertices);
				Existing.Indices = TArray<int32>(Indices);
				Existing.TriangleAreas = TArray<uint8>(TriangleAreas);
				Existing.ProviderPriority = CurrentProviderPriority;
				Existing.ProviderName = CurrentProviderName;
			}
			return true;
		}
		bHasConflict = true;
		Diagnostics.Error(ECustomNavDiagnosticCode::SourceConflict, StageCollect,
			FString::Printf(TEXT("Providers %s and %s report different geometry for the same source (revisions %llu / %llu) with equal priority %d"),
				*Existing.ProviderName, *CurrentProviderName, Existing.Revision, Revision, CurrentProviderPriority),
			Key.ToString());
		return false;
	}

	FCustomNavGeometryChunk& Chunk = Chunks.AddDefaulted_GetRef();
	Chunk.Key = Key;
	Chunk.Revision = Revision;
	Chunk.Vertices = TArray<FVector>(Vertices);
	Chunk.Indices = TArray<int32>(Indices);
	Chunk.TriangleAreas = TArray<uint8>(TriangleAreas);
	Chunk.ProviderPriority = CurrentProviderPriority;
	Chunk.ProviderName = CurrentProviderName;
	ChunkIndexByKey.Add(Key, Chunks.Num() - 1);
	return true;
}

bool FCustomNavGeometryCollector::AddBox(const FCustomNavSourceKey& Key, uint64 Revision, const FBox& Box, uint8 Area)
{
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	CustomNavigationPrivate::MakeBox(Box, Vertices, Indices);
	TArray<uint8> Areas;
	if (Area != CustomNavigation::Area::Walkable)
	{
		Areas.Init(Area, Indices.Num() / 3);
	}
	return AddChunk(Key, Revision, Vertices, Indices, Areas);
}

bool FCustomNavGeometryCollector::AddPlane(const FCustomNavSourceKey& Key, uint64 Revision, const FVector& Center, const FVector2D& HalfExtent, uint8 Area)
{
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	CustomNavigationPrivate::MakePlane(Center, HalfExtent, Vertices, Indices);
	TArray<uint8> Areas;
	if (Area != CustomNavigation::Area::Walkable)
	{
		Areas.Init(Area, Indices.Num() / 3);
	}
	return AddChunk(Key, Revision, Vertices, Indices, Areas);
}

// ---------------------------------------------------------------------------------------------------------------------
// FCustomNavHardcodedGeometryProvider

FCustomNavHardcodedGeometryProvider::FCustomNavHardcodedGeometryProvider(FName InDomain)
	: Domain(InDomain)
	, ObjectId(CityHash64(reinterpret_cast<const char*>(*InDomain.ToString()), InDomain.ToString().Len() * sizeof(TCHAR)))
{
}

uint32 FCustomNavHardcodedGeometryProvider::AddPlane(const FVector& Center, const FVector2D& HalfExtent, uint8 Area)
{
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	CustomNavigationPrivate::MakePlane(Center, HalfExtent, Vertices, Indices);
	TArray<uint8> Areas;
	if (Area != CustomNavigation::Area::Walkable)
	{
		Areas.Init(Area, Indices.Num() / 3);
	}
	return AddTriangles(MoveTemp(Vertices), MoveTemp(Indices), MoveTemp(Areas));
}

uint32 FCustomNavHardcodedGeometryProvider::AddBox(const FBox& Box, uint8 Area)
{
	TArray<FVector> Vertices;
	TArray<int32> Indices;
	CustomNavigationPrivate::MakeBox(Box, Vertices, Indices);
	TArray<uint8> Areas;
	if (Area != CustomNavigation::Area::Walkable)
	{
		Areas.Init(Area, Indices.Num() / 3);
	}
	return AddTriangles(MoveTemp(Vertices), MoveTemp(Indices), MoveTemp(Areas));
}

uint32 FCustomNavHardcodedGeometryProvider::AddTriangles(TArray<FVector> Vertices, TArray<int32> Indices, TArray<uint8> TriangleAreas)
{
	FShape& Shape = Shapes.AddDefaulted_GetRef();
	Shape.PartId = NextPartId++;
	Shape.Vertices = MoveTemp(Vertices);
	Shape.Indices = MoveTemp(Indices);
	Shape.Areas = MoveTemp(TriangleAreas);
	for (const FVector& V : Shape.Vertices)
	{
		if (!V.ContainsNaN())
		{
			Shape.Bounds += V;
		}
	}
	++Revision;
	return Shape.PartId;
}

void FCustomNavHardcodedGeometryProvider::AddDefaultTestScene(const FVector& Center, double HalfSize)
{
	const double S = HalfSize;
	const TArray<FBox> Obstacles = {
		// Tall blocks that cut holes into the floor navmesh (their tops become small separate islands).
		FBox(Center + FVector(-0.50 * S, -0.50 * S, 0.0), Center + FVector(-0.25 * S, -0.10 * S, 300.0)),
		FBox(Center + FVector(0.10 * S, 0.20 * S, 0.0), Center + FVector(0.45 * S, 0.40 * S, 300.0)),
		FBox(Center + FVector(0.30 * S, -0.60 * S, 0.0), Center + FVector(0.40 * S, -0.05 * S, 300.0)),
		// A low wide platform, higher than the default MaxClimb: its top is a separate walkable area.
		FBox(Center + FVector(-0.60 * S, 0.30 * S, 0.0), Center + FVector(-0.20 * S, 0.70 * S, 80.0)),
	};

	// Recast rasterizes surfaces, not volumes: a floor plane running under a closed box would stay walkable inside it
	// (enclosed by the box walls, with the box top as a ceiling). So the floor is built from rectangles around the
	// obstacle footprints instead of one plane.
	TArray<double> Xs = {Center.X - S, Center.X + S};
	TArray<double> Ys = {Center.Y - S, Center.Y + S};
	for (const FBox& Box : Obstacles)
	{
		Xs.Add(Box.Min.X); Xs.Add(Box.Max.X);
		Ys.Add(Box.Min.Y); Ys.Add(Box.Max.Y);
	}
	Xs.Sort(); Ys.Sort();
	TArray<FVector> FloorVertices;
	TArray<int32> FloorIndices;
	for (int32 i = 0; i + 1 < Xs.Num(); ++i)
	{
		for (int32 j = 0; j + 1 < Ys.Num(); ++j)
		{
			if (Xs[i + 1] - Xs[i] < UE_KINDA_SMALL_NUMBER || Ys[j + 1] - Ys[j] < UE_KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FVector2D CellCenter((Xs[i] + Xs[i + 1]) * 0.5, (Ys[j] + Ys[j + 1]) * 0.5);
			const bool bCovered = Obstacles.ContainsByPredicate([&CellCenter](const FBox& Box)
			{
				return CellCenter.X > Box.Min.X && CellCenter.X < Box.Max.X && CellCenter.Y > Box.Min.Y && CellCenter.Y < Box.Max.Y;
			});
			if (bCovered)
			{
				continue;
			}
			const int32 Base = FloorVertices.Num();
			FloorVertices.Add(FVector(Xs[i], Ys[j], Center.Z));
			FloorVertices.Add(FVector(Xs[i + 1], Ys[j], Center.Z));
			FloorVertices.Add(FVector(Xs[i + 1], Ys[j + 1], Center.Z));
			FloorVertices.Add(FVector(Xs[i], Ys[j + 1], Center.Z));
			FloorIndices.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
		}
	}
	AddTriangles(MoveTemp(FloorVertices), MoveTemp(FloorIndices));

	for (const FBox& Box : Obstacles)
	{
		AddBox(Box);
	}
}

void FCustomNavHardcodedGeometryProvider::Clear()
{
	Shapes.Reset();
	++Revision;
}

ECustomNavCollectStatus FCustomNavHardcodedGeometryProvider::CollectGeometry(const FCustomNavGeometryRequest& Request,
                                                                             FCustomNavGeometryCollector& Collector,
                                                                             FCustomNavDiagnostics& Diagnostics)
{
	if (bFailCollection)
	{
		Diagnostics.Error(ECustomNavDiagnosticCode::ProviderFailed, CustomNavigationPrivate::StageCollect,
			TEXT("Hardcoded provider was told to fail (test hook)"), GetDebugName());
		return ECustomNavCollectStatus::Failed;
	}

	for (const FShape& Shape : Shapes)
	{
		// Shapes with NaN-only vertices have invalid bounds; pass them on so validation can report them.
		if (Shape.Bounds.IsValid && Request.WorldBounds.IsValid && !Shape.Bounds.Intersect(Request.WorldBounds))
		{
			continue;
		}
		// AddChunk records its own diagnostic on failure; a malformed chunk fails the whole collection.
		if (!Collector.AddChunk(FCustomNavSourceKey(Domain, ObjectId, Shape.PartId), Revision, Shape.Vertices, Shape.Indices, Shape.Areas))
		{
			return ECustomNavCollectStatus::Failed;
		}
	}
	return ECustomNavCollectStatus::Success;
}

FString FCustomNavHardcodedGeometryProvider::GetDebugName() const
{
	return FString::Printf(TEXT("Hardcoded(%s)"), *Domain.ToString());
}
