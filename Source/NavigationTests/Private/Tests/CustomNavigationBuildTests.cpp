// Created by Stalker7274

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Engine/World.h"
#include "Navigation/CustomNavigationTypes.h"
#include "Navigation/CustomNavigationGeometryProvider.h"
#include "Navigation/CustomNavigationHardcodedProvider.h"
#include "Navigation/CustomNavigationWorldSubsystem.h"
#include "Navigation/Internal/CustomNavigationSnapshot.h"
#include "Navigation/Internal/CustomNavigationRecastBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace CustomNavigationTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Failed builds log warnings by design; tests that provoke them ignore these lines. */
	void IgnoreBuildWarnings(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessage(TEXT("not built"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
		Test.AddExpectedMessage(TEXT("finished with"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	}

	FBox TestBounds(double HalfSize = 1000.0)
	{
		return FBox(FVector(-HalfSize, -HalfSize, -200.0), FVector(HalfSize, HalfSize, 600.0));
	}

	FCustomNavGeometryChunk MakePlaneChunk(double HalfSize, double Z = 0.0, bool bFaceUp = true, uint32 PartId = 1)
	{
		FCustomNavGeometryChunk Chunk;
		Chunk.Key = FCustomNavSourceKey(TEXT("Test"), 1, PartId);
		Chunk.Revision = 1;
		Chunk.Vertices = {
			FVector(-HalfSize, -HalfSize, Z), FVector(HalfSize, -HalfSize, Z),
			FVector(HalfSize, HalfSize, Z), FVector(-HalfSize, HalfSize, Z)};
		Chunk.Indices = bFaceUp ? TArray<int32>{0, 1, 2, 0, 2, 3} : TArray<int32>{0, 2, 1, 0, 3, 2};
		return Chunk;
	}

	FCustomNavBuildToken MakeToken()
	{
		FCustomNavBuildToken Token;
		Token.WorldLifetimeId = 1;
		Token.Key = FCustomNavRegionKey(TEXT("Test"));
		Token.DesiredRevision = 1;
		Token.RequestId = 1;
		return Token;
	}

	/** Snapshot + build in one call (worker logic run synchronously). */
	FCustomNavBuildResult BuildChunks(const TArray<FCustomNavGeometryChunk>& Chunks, const FBox& Bounds, const FCustomNavBuildSettings& Settings = FCustomNavBuildSettings(),
	                                  const FCustomNavCancellation* Cancellation = nullptr)
	{
		FCustomNavBuildSnapshot Snapshot;
		FCustomNavDiagnostics Diagnostics;
		const ECustomNavBuildStatus Status = CustomNavigation::BuildSnapshot(MakeToken(), Bounds, Settings, Chunks, Snapshot, Diagnostics);
		if (Status != ECustomNavBuildStatus::Success)
		{
			FCustomNavBuildResult Result;
			Result.Status = Status;
			Result.Diagnostics = Diagnostics;
			return Result;
		}
		return FCustomNavRecastBuilder::Build(Snapshot, Cancellation);
	}

	ECustomNavBuildStatus Snapshot(const TArray<FCustomNavGeometryChunk>& Chunks, FCustomNavDiagnostics& OutDiagnostics,
	                               const FBox& Bounds = TestBounds(), const FCustomNavBuildSettings& Settings = FCustomNavBuildSettings())
	{
		FCustomNavBuildSnapshot Out;
		return CustomNavigation::BuildSnapshot(MakeToken(), Bounds, Settings, Chunks, Out, OutDiagnostics);
	}

	/** Executor that only queues work; the test decides when workers and completions run. */
	struct FManualQueue
	{
		TArray<TUniqueFunction<void()>> Workers;
		TArray<TUniqueFunction<void()>> Completions;

		int32 RunWorkers()
		{
			int32 Count = 0;
			while (Workers.Num() > 0)
			{
				TUniqueFunction<void()> Work = MoveTemp(Workers[0]);
				Workers.RemoveAt(0);
				Work();
				++Count;
			}
			return Count;
		}

		int32 RunCompletions()
		{
			int32 Count = 0;
			while (Completions.Num() > 0)
			{
				TUniqueFunction<void()> Completion = MoveTemp(Completions[0]);
				Completions.RemoveAt(0);
				Completion();
				++Count;
			}
			return Count;
		}

		void RunAll()
		{
			RunWorkers();
			RunCompletions();
		}
	};

	TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> MakeManualExecutor(const TSharedRef<FManualQueue>& Queue)
	{
		TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> Executor = MakeShared<FCustomNavBuildExecutor, ESPMode::ThreadSafe>();
		Executor->LaunchWorker = [Queue](TUniqueFunction<void()>&& Work) { Queue->Workers.Add(MoveTemp(Work)); };
		Executor->PostToGameThread = [Queue](TUniqueFunction<void()>&& Completion) { Queue->Completions.Add(MoveTemp(Completion)); };
		return Executor;
	}

	/** Test world with the subsystem in manual mode. */
	struct FCoordinatorEnv
	{
		FTestWorldWrapper World;
		UCustomNavigationWorldSubsystem* Subsystem = nullptr;
		TSharedRef<FManualQueue> Queue = MakeShared<FManualQueue>();
		TSharedPtr<FCustomNavHardcodedGeometryProvider> Provider;

		bool Init(FAutomationTestBase& Test)
		{
			if (!World.CreateTestWorld(EWorldType::Game))
			{
				World.ForwardErrorMessages(&Test);
				return false;
			}
			Subsystem = UCustomNavigationWorldSubsystem::Get(World.GetTestWorld());
			if (!Test.TestNotNull(TEXT("Subsystem exists in a game world"), Subsystem))
			{
				return false;
			}
			Subsystem->SetAutoScheduleForTesting(false);
			Subsystem->SetBuildExecutorForTesting(MakeManualExecutor(Queue));
			Provider = MakeShared<FCustomNavHardcodedGeometryProvider>();
			Provider->AddPlane(FVector::ZeroVector, FVector2D(800.0));
			Subsystem->RegisterGeometryProvider(Provider.ToSharedRef());
			return true;
		}

		FCustomNavRegionRequest MakeRequest(FName Id = TEXT("Region")) const
		{
			FCustomNavRegionRequest Request;
			Request.Key = FCustomNavRegionKey(Id, TEXT("Default"));
			Request.WorldBounds = TestBounds();
			return Request;
		}
	};
}

// ---------------------------------------------------------------------------------------------------------------------
// Coordinates and winding
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCoordinatesTest, "CustomNavigation.Build.Coordinates", CustomNavigationTests::Flags)

bool FCustomNavCoordinatesTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	const FVector Origin(100000.25, -50000.5, 300.0);
	const FVector World(100123.5, -49900.25, 512.0);
	const FVector3f Recast = CustomNavigation::WorldToRecast(World, Origin);
	TestEqual(TEXT("Recast x = UE X - origin"), Recast.X, 123.25f);
	TestEqual(TEXT("Recast y = UE Z - origin (up)"), Recast.Y, 212.f);
	TestEqual(TEXT("Recast z = UE Y - origin"), Recast.Z, 100.25f);
	TestTrue(TEXT("Round trip"), CustomNavigation::RecastToWorld(Recast, Origin).Equals(World, 1e-3));

	// An upward UE triangle must face up (+Y) in Recast after the rewind, as rcClearUnwalkableTriangles computes it.
	FCustomNavBuildSnapshot Snapshot;
	FCustomNavDiagnostics Diagnostics;
	TArray<FCustomNavGeometryChunk> Chunks{MakePlaneChunk(500.0)};
	const ECustomNavBuildStatus Status = CustomNavigation::BuildSnapshot(MakeToken(), TestBounds(), FCustomNavBuildSettings(), Chunks, Snapshot, Diagnostics);
	if (!TestEqual(TEXT("Snapshot ok"), Status, ECustomNavBuildStatus::Success))
	{
		return false;
	}
	for (int32 Tri = 0; Tri < Snapshot.NumTriangles(); ++Tri)
	{
		auto V = [&](int32 Corner)
		{
			const int32 I = Snapshot.Indices[Tri * 3 + Corner];
			return FVector3f(Snapshot.Vertices[I * 3], Snapshot.Vertices[I * 3 + 1], Snapshot.Vertices[I * 3 + 2]);
		};
		const FVector3f Normal = FVector3f::CrossProduct(V(1) - V(0), V(2) - V(0));
		TestTrue(FString::Printf(TEXT("Triangle %d faces +Y in Recast"), Tri), Normal.Y > 0.f);
	}
	TestEqual(TEXT("Default area"), Snapshot.TriangleAreas[0], CustomNavigation::Area::Walkable);
	TestTrue(TEXT("Recast bounds ordered"), Snapshot.RecastBoundsMin.X < Snapshot.RecastBoundsMax.X && Snapshot.RecastBoundsMin.Y < Snapshot.RecastBoundsMax.Y);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot validation
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavValidationTest, "CustomNavigation.Build.Validation", CustomNavigationTests::Flags)

bool FCustomNavValidationTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	auto ExpectInvalid = [this](const TCHAR* Name, const TArray<FCustomNavGeometryChunk>& Chunks, ECustomNavDiagnosticCode Code,
	                            const FBox& Bounds = TestBounds(), const FCustomNavBuildSettings& Settings = FCustomNavBuildSettings())
	{
		FCustomNavDiagnostics Diagnostics;
		const ECustomNavBuildStatus Status = Snapshot(Chunks, Diagnostics, Bounds, Settings);
		TestEqual(FString::Printf(TEXT("%s: status"), Name), Status, ECustomNavBuildStatus::InvalidInput);
		TestTrue(FString::Printf(TEXT("%s: code %s in\n%s"), Name, *UEnum::GetValueAsString(Code), *Diagnostics.ToString()), Diagnostics.Contains(Code));
	};

	{
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		Chunk.Vertices[2].Z = NAN;
		ExpectInvalid(TEXT("NaN vertex"), {Chunk}, ECustomNavDiagnosticCode::NonFiniteVertex);
	}
	{
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		Chunk.Indices[4] = 7;
		ExpectInvalid(TEXT("Index out of range"), {Chunk}, ECustomNavDiagnosticCode::IndexOutOfRange);
		Chunk.Indices[4] = -1;
		ExpectInvalid(TEXT("Negative index"), {Chunk}, ECustomNavDiagnosticCode::IndexOutOfRange);
	}
	{
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		Chunk.Indices.Pop();
		ExpectInvalid(TEXT("Index count"), {Chunk}, ECustomNavDiagnosticCode::BadIndexCount);
	}
	{
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		Chunk.TriangleAreas = {CustomNavigation::Area::Walkable};
		ExpectInvalid(TEXT("Area count"), {Chunk}, ECustomNavDiagnosticCode::BadAreaCount);
		Chunk.TriangleAreas = {CustomNavigation::Area::Walkable, 64};
		ExpectInvalid(TEXT("Area id"), {Chunk}, ECustomNavDiagnosticCode::InvalidAreaId);
	}
	ExpectInvalid(TEXT("Uninitialized bounds"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::InvalidBounds, FBox(ForceInit));
	ExpectInvalid(TEXT("Flat bounds"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::InvalidBounds, FBox(FVector(-100, -100, 0), FVector(100, 100, 0)));
	{
		FCustomNavBuildSettings Settings;
		Settings.AgentHeight = 2.f * Settings.CellHeight;
		ExpectInvalid(TEXT("Agent height"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::InvalidSettings, TestBounds(), Settings);
		Settings = FCustomNavBuildSettings();
		Settings.CellSize = 0.f;
		ExpectInvalid(TEXT("Cell size"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::InvalidSettings, TestBounds(), Settings);
		Settings = FCustomNavBuildSettings();
		Settings.MaxVertsPerPoly = 7;
		ExpectInvalid(TEXT("Verts per poly"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::InvalidSettings, TestBounds(), Settings);
		Settings = FCustomNavBuildSettings();
		Settings.MaxGridCells = 100;
		ExpectInvalid(TEXT("Grid limit"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::GridTooLarge, TestBounds(), Settings);
		Settings = FCustomNavBuildSettings();
		Settings.MaxInputTriangles = 1;
		ExpectInvalid(TEXT("Triangle limit"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::TooManyTriangles, TestBounds(), Settings);
		Settings = FCustomNavBuildSettings();
		Settings.MaxInputVertices = 3;
		ExpectInvalid(TEXT("Vertex limit"), {MakePlaneChunk(500.0)}, ECustomNavDiagnosticCode::TooManyVertices, TestBounds(), Settings);
	}
	{
		FCustomNavDiagnostics Diagnostics;
		TestEqual(TEXT("No chunks -> NoGeometry"), Snapshot({}, Diagnostics), ECustomNavBuildStatus::NoGeometry);
		TestTrue(TEXT("NoGeometry diagnostic"), Diagnostics.Contains(ECustomNavDiagnosticCode::NoGeometry));
	}
	{
		// One degenerate triangle among good ones: dropped with a warning, the rest is kept.
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		Chunk.Indices.Append({1, 1, 2});
		FCustomNavDiagnostics Diagnostics;
		FCustomNavBuildSnapshot Out;
		const ECustomNavBuildStatus Status = CustomNavigation::BuildSnapshot(MakeToken(), TestBounds(), FCustomNavBuildSettings(), {Chunk}, Out, Diagnostics);
		TestEqual(TEXT("Degenerate: still success"), Status, ECustomNavBuildStatus::Success);
		TestEqual(TEXT("Degenerate: dropped"), Out.NumTriangles(), 2);
		TestTrue(TEXT("Degenerate: warning"), Diagnostics.Contains(ECustomNavDiagnosticCode::DegenerateTriangleDropped));
		TestFalse(TEXT("Degenerate: no error"), Diagnostics.HasErrors());
	}
	{
		FCustomNavGeometryChunk Chunk;
		Chunk.Key = FCustomNavSourceKey(TEXT("Test"), 2);
		Chunk.Vertices = {FVector(0, 0, 0), FVector(100, 0, 0), FVector(200, 0, 0)};
		Chunk.Indices = {0, 1, 2};
		FCustomNavDiagnostics Diagnostics;
		TestEqual(TEXT("Only collinear triangles -> NoGeometry"), Snapshot({Chunk}, Diagnostics), ECustomNavBuildStatus::NoGeometry);
	}
	{
		// Diagnostics stay bounded for huge broken input.
		FCustomNavGeometryChunk Chunk = MakePlaneChunk(500.0);
		for (int32 i = 0; i < 1000; ++i)
		{
			Chunk.Indices.Append({0, 1, 99});
		}
		FCustomNavDiagnostics Diagnostics;
		Snapshot({Chunk}, Diagnostics);
		TestTrue(TEXT("Bounded entries"), Diagnostics.Entries.Num() <= FCustomNavDiagnostics::MaxEntries);
		TestTrue(TEXT("Dropped are counted"), Diagnostics.DroppedCount > 0 && Diagnostics.ErrorCount >= 1000);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Collector duplicate handling
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCollectorTest, "CustomNavigation.Build.Collector", CustomNavigationTests::Flags)

bool FCustomNavCollectorTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	const FCustomNavSourceKey Key(TEXT("Shared"), 42);
	const FBox SmallBox(FVector(0, 0, 0), FVector(100, 100, 100));
	const FBox BigBox(FVector(0, 0, 0), FVector(200, 200, 100));
	{
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		Collector.SetCurrentProvider(TEXT("A"), 0);
		TestTrue(TEXT("First add"), Collector.AddBox(Key, 1, SmallBox));
		Collector.SetCurrentProvider(TEXT("B"), 0);
		TestTrue(TEXT("Identical duplicate accepted"), Collector.AddBox(Key, 1, SmallBox));
		TestEqual(TEXT("Kept once"), Collector.GetChunks().Num(), 1);
		TestTrue(TEXT("Duplicate reported"), Diagnostics.Contains(ECustomNavDiagnosticCode::DuplicateSourceSkipped));
		TestFalse(TEXT("No conflict"), Collector.HasConflict());
	}
	{
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		Collector.SetCurrentProvider(TEXT("Low"), 0);
		Collector.AddBox(Key, 1, SmallBox);
		Collector.SetCurrentProvider(TEXT("High"), 10);
		TestTrue(TEXT("Higher priority replaces"), Collector.AddBox(Key, 2, BigBox));
		TestEqual(TEXT("Still one chunk"), Collector.GetChunks().Num(), 1);
		TestEqual(TEXT("High wins"), Collector.GetChunks()[0].ProviderName, FString(TEXT("High")));
		Collector.SetCurrentProvider(TEXT("Lower"), 5);
		TestTrue(TEXT("Lower priority ignored"), Collector.AddBox(Key, 3, SmallBox));
		TestEqual(TEXT("High still wins"), Collector.GetChunks()[0].ProviderName, FString(TEXT("High")));
		TestFalse(TEXT("No conflict"), Collector.HasConflict());
	}
	{
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		Collector.SetCurrentProvider(TEXT("A"), 0);
		Collector.AddBox(Key, 1, SmallBox);
		Collector.SetCurrentProvider(TEXT("B"), 0);
		TestFalse(TEXT("Same priority different payload fails"), Collector.AddBox(Key, 2, BigBox));
		TestTrue(TEXT("Conflict flagged"), Collector.HasConflict());
		TestTrue(TEXT("Conflict reported"), Diagnostics.Contains(ECustomNavDiagnosticCode::SourceConflict));
	}
	{
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		const TArray<FVector> Verts{FVector::ZeroVector, FVector::XAxisVector, FVector::YAxisVector};
		const TArray<int32> BadIndices{0, 1};
		TestFalse(TEXT("Malformed chunk rejected"), Collector.AddChunk(FCustomNavSourceKey(TEXT("X"), 1), 1, Verts, BadIndices));
		TestTrue(TEXT("Malformed reported"), Diagnostics.HasErrors());
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recast builder (synchronous, no world)
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavRecastBuilderTest, "CustomNavigation.Build.Recast", CustomNavigationTests::Flags)

bool FCustomNavRecastBuilderTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	const FCustomNavBuildSettings Settings;
	const double HalfSize = 800.0;

	// Plane -> mesh
	{
		const FCustomNavBuildResult Result = BuildChunks({MakePlaneChunk(HalfSize)}, TestBounds());
		if (!TestEqual(FString::Printf(TEXT("Plane builds\n%s"), *Result.Diagnostics.ToString()), Result.Status, ECustomNavBuildStatus::Success)
			|| !TestTrue(TEXT("Mesh set"), Result.Mesh.IsValid()))
		{
			return false;
		}
		const FCustomNavRegionMesh& Mesh = *Result.Mesh;
		TestTrue(TEXT("Has polygons"), Mesh.NumPolygons() > 0);
		TestEqual(TEXT("Offsets size"), Mesh.PolyOffsets.Num(), Mesh.NumPolygons() + 1);
		TestEqual(TEXT("Area per poly"), Mesh.PolyAreas.Num(), Mesh.NumPolygons());

		// Eroded by the agent radius on every side.
		const double Expected = FMath::Square(2.0 * (HalfSize - Settings.AgentRadius));
		const double Area = Mesh.ComputeProjectedArea();
		TestTrue(FString::Printf(TEXT("Area %.0f close to %.0f"), Area, Expected), FMath::Abs(Area - Expected) < Expected * 0.08);

		for (const FVector& V : Mesh.PolyVertices)
		{
			if (!TestTrue(TEXT("Vertex near the plane (world space)"), FMath::Abs(V.Z) <= Settings.CellHeight * 2.0 && FMath::Abs(V.X) <= HalfSize && FMath::Abs(V.Y) <= HalfSize))
			{
				break;
			}
		}
		TestTrue(TEXT("Detail mesh built"), Mesh.DetailTriangles.Num() > 0 && Mesh.DetailTriangles.Num() == Mesh.DetailTrianglePolys.Num() * 3);
		for (int32 Tri = 0; Tri < Mesh.DetailTrianglePolys.Num(); ++Tri)
		{
			const FVector& A = Mesh.DetailVertices[Mesh.DetailTriangles[Tri * 3]];
			const FVector N = FVector::CrossProduct(Mesh.DetailVertices[Mesh.DetailTriangles[Tri * 3 + 1]] - A, Mesh.DetailVertices[Mesh.DetailTriangles[Tri * 3 + 2]] - A);
			if (!TestTrue(TEXT("Detail triangle faces +Z"), N.Z > 0.0))
			{
				break;
			}
		}
		for (int32 Poly = 0; Poly < Mesh.NumPolygons(); ++Poly)
		{
			double Twice = 0.0;
			const int32 Start = Mesh.PolyOffsets[Poly];
			const int32 Count = Mesh.NumPolygonVerts(Poly);
			for (int32 i = 0; i < Count; ++i)
			{
				const FVector& P = Mesh.PolyVertices[Mesh.PolyIndices[Start + i]];
				const FVector& Q = Mesh.PolyVertices[Mesh.PolyIndices[Start + (i + 1) % Count]];
				Twice += P.X * Q.Y - Q.X * P.Y;
			}
			if (!TestTrue(TEXT("Polygon is CCW from +Z"), Twice > 0.0))
			{
				break;
			}
		}
		TestTrue(TEXT("Timings recorded"), Mesh.StageTimingsMs.Contains(TEXT("Rasterize")));
	}

	// The same plane far from the origin (large world coordinates) still works.
	{
		FCustomNavGeometryChunk Far = MakePlaneChunk(HalfSize);
		const FVector Offset(2000000.0, -3000000.0, 50000.0);
		for (FVector& V : Far.Vertices)
		{
			V += Offset;
		}
		const FCustomNavBuildResult Result = BuildChunks({Far}, TestBounds().ShiftBy(Offset));
		TestEqual(TEXT("Far plane builds"), Result.Status, ECustomNavBuildStatus::Success);
		if (Result.Mesh.IsValid())
		{
			TestTrue(TEXT("Far mesh in world space"), Result.Mesh->MeshBounds.GetCenter().Equals(Offset, 50.0));
		}
	}

	// Downward plane -> nothing walkable
	{
		const FCustomNavBuildResult Result = BuildChunks({MakePlaneChunk(HalfSize, 0.0, false)}, TestBounds());
		TestEqual(TEXT("Downward plane"), Result.Status, ECustomNavBuildStatus::NoWalkableSurface);
		TestTrue(TEXT("Downward diagnostic"), Result.Diagnostics.Contains(ECustomNavDiagnosticCode::NoWalkableSurface));
		TestFalse(TEXT("No mesh"), Result.Mesh.IsValid());
	}

	// Vertical wall only -> nothing walkable
	{
		FCustomNavGeometryChunk Wall;
		Wall.Key = FCustomNavSourceKey(TEXT("Test"), 3);
		Wall.Vertices = {FVector(-500, 0, 0), FVector(500, 0, 0), FVector(500, 0, 300), FVector(-500, 0, 300)};
		Wall.Indices = {0, 1, 2, 0, 2, 3};
		const FCustomNavBuildResult Result = BuildChunks({Wall}, TestBounds());
		TestEqual(TEXT("Wall only"), Result.Status, ECustomNavBuildStatus::NoWalkableSurface);
	}

	// Null-area plane -> nothing walkable
	{
		FCustomNavGeometryChunk Plane = MakePlaneChunk(HalfSize);
		Plane.TriangleAreas = {CustomNavigation::Area::Null, CustomNavigation::Area::Null};
		TestEqual(TEXT("Null area plane"), BuildChunks({Plane}, TestBounds()).Status, ECustomNavBuildStatus::NoWalkableSurface);
	}

	// Obstacle reduces the walkable area
	{
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		Collector.AddPlane(FCustomNavSourceKey(TEXT("Test"), 1), 1, FVector::ZeroVector, FVector2D(HalfSize));
		const FCustomNavBuildResult Plain = BuildChunks(Collector.GetChunks(), TestBounds());
		Collector.AddBox(FCustomNavSourceKey(TEXT("Test"), 2), 1, FBox(FVector(-200, -200, 0), FVector(200, 200, 300)));
		const FCustomNavBuildResult Blocked = BuildChunks(Collector.GetChunks(), TestBounds());
		if (TestEqual(TEXT("Plain ok"), Plain.Status, ECustomNavBuildStatus::Success) && TestEqual(TEXT("Blocked ok"), Blocked.Status, ECustomNavBuildStatus::Success))
		{
			const double PlainArea = Plain.Mesh->ComputeProjectedArea();
			const double BlockedArea = Blocked.Mesh->ComputeProjectedArea();
			// 300 cm tall box (unclimbable): its footprint plus the radius ring is lost, its top (400x400 minus erosion) is gained back.
			TestTrue(FString::Printf(TEXT("Obstacle changes area (%.0f vs %.0f)"), BlockedArea, PlainArea), FMath::Abs(PlainArea - BlockedArea) > 20000.0);
		}
	}

	// Default hardcoded scene builds
	{
		TSharedRef<FCustomNavHardcodedGeometryProvider> Provider = MakeShared<FCustomNavHardcodedGeometryProvider>();
		Provider->AddDefaultTestScene(FVector::ZeroVector, 2000.0);
		FCustomNavDiagnostics Diagnostics;
		FCustomNavGeometryCollector Collector(Diagnostics);
		FCustomNavGeometryRequest Request;
		Request.WorldBounds = TestBounds(2200.0);
		TestEqual(TEXT("Scene collects"), Provider->CollectGeometry(Request, Collector, Diagnostics), ECustomNavCollectStatus::Success);
		const FCustomNavBuildResult Result = BuildChunks(Collector.GetChunks(), TestBounds(2200.0));
		TestEqual(FString::Printf(TEXT("Scene builds\n%s"), *Result.Diagnostics.ToString()), Result.Status, ECustomNavBuildStatus::Success);
		if (Result.Mesh.IsValid())
		{
			AddInfo(FString::Printf(TEXT("Test scene: %d polys, %d detail tris, %.2f ms"), Result.Mesh->NumPolygons(), Result.Mesh->DetailTriangles.Num() / 3, Result.TotalBuildMs));
		}
	}

	// Pre-cancelled
	{
		FCustomNavCancellation Cancellation;
		Cancellation.Cancel();
		const FCustomNavBuildResult Result = BuildChunks({MakePlaneChunk(HalfSize)}, TestBounds(), Settings, &Cancellation);
		TestEqual(TEXT("Cancelled"), Result.Status, ECustomNavBuildStatus::Cancelled);
		TestFalse(TEXT("Cancelled has no mesh"), Result.Mesh.IsValid());
	}

	// Malformed snapshot handed directly to the builder does not crash
	{
		FCustomNavBuildSnapshot Broken;
		Broken.Vertices = {0.f, 0.f};
		Broken.Indices = {0, 1, 2};
		const FCustomNavBuildResult Result = FCustomNavRecastBuilder::Build(Broken);
		TestEqual(TEXT("Malformed snapshot"), Result.Status, ECustomNavBuildStatus::InvalidInput);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Coordinator (manual executor)
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCoordinatorPublishTest, "CustomNavigation.Coordinator.Publish", CustomNavigationTests::Flags)

bool FCustomNavCoordinatorPublishTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	FCoordinatorEnv Env;
	if (!Env.Init(*this))
	{
		return false;
	}
	UCustomNavigationWorldSubsystem& Sub = *Env.Subsystem;
	const FCustomNavRegionRequest Request = Env.MakeRequest();

	int32 FinishedCount = 0;
	Sub.OnRegionBuildFinished.AddLambda([&FinishedCount](const FCustomNavRegionKey&, const FCustomNavRegionStatus&) { ++FinishedCount; });

	const uint64 Revision = Sub.RequestRegionBuild(Request);
	TestTrue(TEXT("Revision returned"), Revision > 0);
	TestEqual(TEXT("Pending"), Sub.GetRegionStatus(Request.Key).State, ECustomNavRegionState::Pending);
	TestEqual(TEXT("Nothing launched before flush"), Sub.GetLaunchedJobCount(), 0);

	Sub.FlushPendingBuilds();
	TestEqual(TEXT("One job launched"), Sub.GetLaunchedJobCount(), 1);
	TestEqual(TEXT("Building"), Sub.GetRegionStatus(Request.Key).State, ECustomNavRegionState::Building);
	TestTrue(TEXT("Job running"), Sub.IsBuildRunning());
	TestFalse(TEXT("Nothing published yet"), Sub.GetPublishedRegion(Request.Key).IsValid());

	// Sharing: an identical request while building starts nothing new.
	TestEqual(TEXT("Identical request shares revision"), Sub.RequestRegionBuild(Request), Revision);
	Sub.FlushPendingBuilds();
	TestEqual(TEXT("Still one job"), Sub.GetLaunchedJobCount(), 1);

	Env.Queue->RunWorkers();
	TestFalse(TEXT("Worker alone does not publish"), Sub.GetPublishedRegion(Request.Key).IsValid());
	Env.Queue->RunCompletions();

	const FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
	TestEqual(FString::Printf(TEXT("Outcome\n%s"), *Status.LastDiagnostics.ToString()), Status.LastOutcome, ECustomNavBuildStatus::Success);
	TestEqual(TEXT("Ready"), Status.State, ECustomNavRegionState::Ready);
	TestEqual(TEXT("Published revision"), Status.PublishedRevision, Revision);
	TestEqual(TEXT("Delegate fired once"), FinishedCount, 1);
	FCustomNavRegionMeshPtr Mesh = Sub.GetPublishedRegion(Request.Key);
	TestTrue(TEXT("Mesh published"), Mesh.IsValid() && Mesh->NumPolygons() > 0);
	TestFalse(TEXT("Slot free"), Sub.IsBuildRunning());

	// Ready + identical request: shared, nothing new.
	TestEqual(TEXT("Identical request after ready"), Sub.RequestRegionBuild(Request), Revision);
	Sub.FlushPendingBuilds();
	TestEqual(TEXT("No rebuild"), Sub.GetLaunchedJobCount(), 1);

	// Geometry change outside the region does not dirty it; inside does.
	Sub.InvalidateGeometry(FBox(FVector(50000, 50000, 0), FVector(50100, 50100, 100)));
	TestEqual(TEXT("Far change ignored"), Sub.GetRegionStatus(Request.Key).State, ECustomNavRegionState::Ready);
	Sub.InvalidateGeometry(FBox(FVector(0, 0, 0), FVector(100, 100, 100)));
	const FCustomNavRegionStatus Dirty = Sub.GetRegionStatus(Request.Key);
	TestEqual(TEXT("Near change dirties"), Dirty.State, ECustomNavRegionState::Pending);
	TestTrue(TEXT("Old mesh is stale"), Dirty.bPublishedIsStale);
	TestFalse(TEXT("Stale hidden by default"), Sub.GetPublishedRegion(Request.Key).IsValid());
	TestTrue(TEXT("Stale available on request"), Sub.GetPublishedRegion(Request.Key, true) == Mesh);

	// Mesh outlives the world.
	Env.World.DestroyTestWorld(false);
	TestTrue(TEXT("Mesh held by consumer survives"), Mesh.IsValid() && Mesh->NumPolygons() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCoordinatorStaleTest, "CustomNavigation.Coordinator.StaleResult", CustomNavigationTests::Flags)

bool FCustomNavCoordinatorStaleTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	FCoordinatorEnv Env;
	if (!Env.Init(*this))
	{
		return false;
	}
	UCustomNavigationWorldSubsystem& Sub = *Env.Subsystem;
	const FCustomNavRegionRequest Request = Env.MakeRequest();

	const uint64 RevisionN = Sub.RequestRegionBuild(Request);
	Sub.FlushPendingBuilds();
	TestEqual(TEXT("Job N launched"), Sub.GetLaunchedJobCount(), 1);

	// Geometry changes while job N is in flight.
	Env.Provider->AddBox(FBox(FVector(-200, -200, 0), FVector(200, 200, 300)));
	Sub.InvalidateAll();
	const uint64 RevisionN1 = Sub.GetRegionStatus(Request.Key).DesiredRevision;
	TestTrue(TEXT("Revision advanced"), RevisionN1 > RevisionN);

	// One job per world: N+1 waits for the slot.
	Sub.FlushPendingBuilds();
	TestEqual(TEXT("N+1 not launched while N runs"), Sub.GetLaunchedJobCount(), 1);

	// Complete N: must be discarded.
	Env.Queue->RunAll();
	FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
	TestEqual(TEXT("N discarded"), Status.DiscardedCompletions, 1);
	TestEqual(TEXT("Nothing published from N"), Status.PublishedRevision, uint64(0));
	TestEqual(TEXT("Still pending"), Status.State, ECustomNavRegionState::Pending);

	// N+1 runs and publishes.
	Sub.FlushPendingBuilds();
	TestEqual(TEXT("N+1 launched"), Sub.GetLaunchedJobCount(), 2);
	Env.Queue->RunAll();
	Status = Sub.GetRegionStatus(Request.Key);
	TestEqual(TEXT("N+1 published"), Status.PublishedRevision, RevisionN1);
	TestEqual(TEXT("Ready"), Status.State, ECustomNavRegionState::Ready);

	// A completion for a removed region is discarded without crashing.
	Sub.InvalidateAll();
	Sub.FlushPendingBuilds();
	Sub.RemoveRegion(Request.Key);
	Env.Queue->RunAll();
	TestFalse(TEXT("Removed region not resurrected"), Sub.HasRegion(Request.Key));
	TestFalse(TEXT("Slot freed after discarded completion"), Sub.IsBuildRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCoordinatorTeardownTest, "CustomNavigation.Coordinator.Teardown", CustomNavigationTests::Flags)

bool FCustomNavCoordinatorTeardownTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	FCoordinatorEnv Env;
	if (!Env.Init(*this))
	{
		return false;
	}
	TSharedRef<FManualQueue> Queue = Env.Queue;
	UCustomNavigationWorldSubsystem* Sub = Env.Subsystem;
	TWeakObjectPtr<UCustomNavigationWorldSubsystem> WeakSub(Sub);

	int32 FinishedCount = 0;
	Sub->OnRegionBuildFinished.AddLambda([&FinishedCount](const FCustomNavRegionKey&, const FCustomNavRegionStatus&) { ++FinishedCount; });

	Sub->RequestRegionBuild(Env.MakeRequest());
	Sub->FlushPendingBuilds();
	TestEqual(TEXT("Job launched"), Queue->Workers.Num(), 1);

	// Destroy the world (and drop the provider) while the job is in flight.
	Env.World.DestroyTestWorld(/*bForceGarbageCollect*/ false);
	Env.Provider.Reset();
	TestTrue(TEXT("Subsystem closing"), !WeakSub.IsValid() || WeakSub->IsClosing());

	// The worker still has valid input (it owns its snapshot) and finishes; the completion is dropped.
	TestEqual(TEXT("Worker ran"), Queue->RunWorkers(), 1);
	TestEqual(TEXT("Completion delivered"), Queue->RunCompletions(), 1);
	TestEqual(TEXT("No broadcast after teardown"), FinishedCount, 0);
	if (WeakSub.IsValid())
	{
		TestFalse(TEXT("Nothing published after teardown"), WeakSub->HasRegion(FCustomNavRegionKey(TEXT("Region"), TEXT("Default"))));
	}

	// Same again, but GC the subsystem before the completion runs.
	FCoordinatorEnv Env2;
	if (!Env2.Init(*this))
	{
		return false;
	}
	TWeakObjectPtr<UCustomNavigationWorldSubsystem> WeakSub2(Env2.Subsystem);
	Env2.Subsystem->RequestRegionBuild(Env2.MakeRequest());
	Env2.Subsystem->FlushPendingBuilds();
	Env2.Queue->RunWorkers();
	Env2.World.DestroyTestWorld(/*bForceGarbageCollect*/ true);
	TestFalse(TEXT("Subsystem collected"), WeakSub2.IsValid());
	TestEqual(TEXT("Completion after GC is a no-op"), Env2.Queue->RunCompletions(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavCoordinatorFailuresTest, "CustomNavigation.Coordinator.Failures", CustomNavigationTests::Flags)

bool FCustomNavCoordinatorFailuresTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	IgnoreBuildWarnings(*this);
	FCoordinatorEnv Env;
	if (!Env.Init(*this))
	{
		return false;
	}
	UCustomNavigationWorldSubsystem& Sub = *Env.Subsystem;

	// Invalid settings: rejected on the game thread, no worker launched.
	{
		FCustomNavRegionRequest Request = Env.MakeRequest(TEXT("BadSettings"));
		Request.Settings.AgentHeight = 1.f;
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		const FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
		TestEqual(TEXT("Invalid settings outcome"), Status.LastOutcome, ECustomNavBuildStatus::InvalidInput);
		TestEqual(TEXT("Invalid settings state"), Status.State, ECustomNavRegionState::Failed);
		TestTrue(TEXT("Invalid settings diagnostic"), Status.LastDiagnostics.Contains(ECustomNavDiagnosticCode::InvalidSettings));
		TestEqual(TEXT("No job for invalid input"), Sub.GetLaunchedJobCount(), 0);
		Sub.RemoveRegion(Request.Key);
	}

	// Invalid geometry from a provider.
	{
		TSharedRef<FCustomNavHardcodedGeometryProvider> Bad = MakeShared<FCustomNavHardcodedGeometryProvider>(TEXT("Bad"));
		Bad->AddTriangles({FVector(0, 0, 0), FVector(100, 0, 0), FVector(0, 100, NAN)}, {0, 1, 2});
		const FCustomNavGeometryProviderHandle Handle = Sub.RegisterGeometryProvider(Bad);
		const FCustomNavRegionRequest Request = Env.MakeRequest(TEXT("BadGeometry"));
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		const FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
		TestEqual(TEXT("Bad geometry outcome"), Status.LastOutcome, ECustomNavBuildStatus::InvalidInput);
		TestTrue(TEXT("Bad geometry diagnostic"), Status.LastDiagnostics.Contains(ECustomNavDiagnosticCode::NonFiniteVertex));
		TestEqual(TEXT("No job for bad geometry"), Sub.GetLaunchedJobCount(), 0);
		Sub.UnregisterGeometryProvider(Handle);
		Sub.RemoveRegion(Request.Key);
	}

	// Provider failure.
	{
		Env.Provider->SetFailCollection(true);
		const FCustomNavRegionRequest Request = Env.MakeRequest(TEXT("ProviderFails"));
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		const FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
		TestEqual(TEXT("Provider failure outcome"), Status.LastOutcome, ECustomNavBuildStatus::CollectionFailed);
		TestTrue(TEXT("Provider failure diagnostic"), Status.LastDiagnostics.Contains(ECustomNavDiagnosticCode::ProviderFailed));
		Env.Provider->SetFailCollection(false);

		// Identical request after a failure retries.
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		Env.Queue->RunAll();
		TestEqual(TEXT("Retry succeeds"), Sub.GetRegionStatus(Request.Key).LastOutcome, ECustomNavBuildStatus::Success);
		Sub.RemoveRegion(Request.Key);
	}

	// Two providers seeing the same source.
	{
		const int32 JobsBefore = Sub.GetLaunchedJobCount();
		TSharedRef<FCustomNavHardcodedGeometryProvider> Twin = MakeShared<FCustomNavHardcodedGeometryProvider>();
		Twin->AddPlane(FVector::ZeroVector, FVector2D(800.0)); // same domain, part id and payload as Env.Provider
		const FCustomNavGeometryProviderHandle TwinHandle = Sub.RegisterGeometryProvider(Twin);
		const FCustomNavRegionRequest Request = Env.MakeRequest(TEXT("Twins"));
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		Env.Queue->RunAll();
		FCustomNavRegionStatus Status = Sub.GetRegionStatus(Request.Key);
		TestEqual(TEXT("Identical duplicates build"), Status.LastOutcome, ECustomNavBuildStatus::Success);
		TestTrue(TEXT("Duplicate reported"), Status.LastDiagnostics.Contains(ECustomNavDiagnosticCode::DuplicateSourceSkipped));
		TestEqual(TEXT("One job"), Sub.GetLaunchedJobCount(), JobsBefore + 1);

		// Same key, different payload, same priority -> conflict.
		// Part ids keep growing after Clear(), so use a fresh provider to reproduce the same key.
		Sub.UnregisterGeometryProvider(TwinHandle);
		TSharedRef<FCustomNavHardcodedGeometryProvider> Rival = MakeShared<FCustomNavHardcodedGeometryProvider>();
		Rival->AddPlane(FVector::ZeroVector, FVector2D(500.0));
		const FCustomNavGeometryProviderHandle RivalHandle = Sub.RegisterGeometryProvider(Rival);
		Sub.FlushPendingBuilds();
		Status = Sub.GetRegionStatus(Request.Key);
		TestEqual(TEXT("Conflict outcome"), Status.LastOutcome, ECustomNavBuildStatus::CollectionFailed);
		TestTrue(TEXT("Conflict diagnostic"), Status.LastDiagnostics.Contains(ECustomNavDiagnosticCode::SourceConflict));
		TestTrue(TEXT("Last good mesh still available as stale"), Sub.GetPublishedRegion(Request.Key, true).IsValid());
		Sub.UnregisterGeometryProvider(RivalHandle);
	}

	// Empty world.
	{
		Env.Provider->Clear();
		const FCustomNavRegionRequest Request = Env.MakeRequest(TEXT("Empty"));
		Sub.RequestRegionBuild(Request);
		Sub.FlushPendingBuilds();
		TestEqual(TEXT("No geometry outcome"), Sub.GetRegionStatus(Request.Key).LastOutcome, ECustomNavBuildStatus::NoGeometry);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Real async path (UE::Tasks worker + game thread completion)
// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCustomNavAsyncSmokeTest, "CustomNavigation.Coordinator.AsyncSmoke", CustomNavigationTests::Flags)

bool FCustomNavAsyncSmokeTest::RunTest(const FString& Parameters)
{
	using namespace CustomNavigationTests;
	struct FState
	{
		FTestWorldWrapper World;
		UCustomNavigationWorldSubsystem* Subsystem = nullptr;
		FCustomNavRegionKey Key{TEXT("Smoke"), TEXT("Default")};
		uint64 Revision = 0;
		double StartTime = 0.0;
		bool bWorkerOffGameThread = false;
	};
	TSharedRef<FState> State = MakeShared<FState>();

	if (!State->World.CreateTestWorld(EWorldType::Game))
	{
		State->World.ForwardErrorMessages(this);
		return false;
	}
	State->Subsystem = UCustomNavigationWorldSubsystem::Get(State->World.GetTestWorld());
	if (!TestNotNull(TEXT("Subsystem"), State->Subsystem))
	{
		return false;
	}

	// Default executor, wrapped only to observe the worker thread.
	TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> Default = FCustomNavBuildExecutor::MakeDefault();
	TSharedRef<FCustomNavBuildExecutor, ESPMode::ThreadSafe> Observed = MakeShared<FCustomNavBuildExecutor, ESPMode::ThreadSafe>();
	Observed->PostToGameThread = Default->PostToGameThread;
	TWeakPtr<FState> WeakState = State;
	Observed->LaunchWorker = [Default, StatePtr = &State.Get()](TUniqueFunction<void()>&& Work)
	{
		Default->LaunchWorker([StatePtr, Work = MoveTemp(Work)]() mutable
		{
			// Written on the worker, read on the game thread after the completion (task-graph ordering).
			StatePtr->bWorkerOffGameThread = !IsInGameThread();
			Work();
		});
	};
	State->Subsystem->SetBuildExecutorForTesting(Observed);

	TSharedRef<FCustomNavHardcodedGeometryProvider> Provider = MakeShared<FCustomNavHardcodedGeometryProvider>();
	Provider->AddDefaultTestScene(FVector::ZeroVector, 2000.0);
	State->Subsystem->RegisterGeometryProvider(Provider);

	FCustomNavRegionRequest Request;
	Request.Key = State->Key;
	Request.WorldBounds = TestBounds(2200.0);
	State->Revision = State->Subsystem->RequestRegionBuild(Request);
	State->StartTime = FPlatformTime::Seconds();

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		const FCustomNavRegionStatus Status = State->Subsystem->GetRegionStatus(State->Key);
		const bool bDone = Status.State == ECustomNavRegionState::Ready || Status.State == ECustomNavRegionState::Failed;
		const bool bTimedOut = FPlatformTime::Seconds() - State->StartTime > 20.0;
		if (!bDone && !bTimedOut)
		{
			return false;
		}
		TestFalse(TEXT("Async build finished in time"), bTimedOut);
		TestEqual(FString::Printf(TEXT("Async outcome\n%s"), *Status.LastDiagnostics.ToString()), Status.LastOutcome, ECustomNavBuildStatus::Success);
		TestEqual(TEXT("Async revision"), Status.PublishedRevision, State->Revision);
		TestTrue(TEXT("Worker ran off the game thread"), State->bWorkerOffGameThread);
		const FCustomNavRegionMeshPtr Mesh = State->Subsystem->GetPublishedRegion(State->Key);
		TestTrue(TEXT("Async mesh"), Mesh.IsValid() && Mesh->NumPolygons() > 0);
		State->World.DestroyTestWorld(false);
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
