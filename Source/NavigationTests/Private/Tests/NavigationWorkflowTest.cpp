#include "Misc/AutomationTest.h"
#include "Recast.h"
#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRecastHeightfieldWorkflowTest,
	"CustomNavigation.Workflow.Recast.Heightfield",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FRecastHeightfieldWorkflowTest::RunTest(const FString& Parameters)
{
	rcContext Context(false);
	const float MinBounds[3] = {0.0f, 0.0f, 0.0f};
	const float MaxBounds[3] = {2.0f, 4.0f, 2.0f};
	int Width = 0;
	int Height = 0;
	rcCalcGridSize(MinBounds, MaxBounds, 1.0f, &Width, &Height);
	TestEqual(TEXT("Grid width"), Width, 2);
	TestEqual(TEXT("Grid height"), Height, 2);

	rcHeightfield Field;
	if (!TestTrue(TEXT("Create heightfield"), rcCreateHeightfield(&Context, Field, Width, Height, MinBounds, MaxBounds, 1.0f, 1.0f)))
	{
		return false;
	}

	if (!TestTrue(TEXT("Add first walkable span"), rcAddSpan(&Context, Field, 0, 0, 0, 1, RC_WALKABLE_AREA, 0)) ||
		!TestTrue(TEXT("Add second walkable span"), rcAddSpan(&Context, Field, 1, 0, 0, 1, RC_WALKABLE_AREA, 0)) ||
		!TestTrue(TEXT("Add unwalkable span"), rcAddSpan(&Context, Field, 0, 1, 0, 1, RC_NULL_AREA, 0)))
	{
		return false;
	}

	rcCompactHeightfield Compact;
	if (!TestTrue(TEXT("Build compact heightfield"), rcBuildCompactHeightfield(&Context, 2, 1, Field, Compact)))
	{
		return false;
	}
	TestEqual(TEXT("Only walkable spans are retained"), Compact.spanCount, 2);
	TestEqual(TEXT("First cell has one span"), static_cast<int32>(Compact.cells[0].count), 1);
	TestEqual(TEXT("Second cell has one span"), static_cast<int32>(Compact.cells[1].count), 1);
	TestEqual(TEXT("Unwalkable cell has no spans"), static_cast<int32>(Compact.cells[2].count), 0);
	TestEqual(TEXT("Empty cell has no spans"), static_cast<int32>(Compact.cells[3].count), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDetourNavigationWorkflowTest,
	"CustomNavigation.Workflow.Detour.Navigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FDetourNavigationWorkflowTest::RunTest(const FString& Parameters)
{
	const unsigned short Vertices[] = {0, 0, 0, 0, 0, 4, 4, 0, 4, 4, 0, 0};
	const unsigned short Polygons[] = {0, 1, 2, 3, 0xffff, 0xffff, 0xffff, 0xffff};
	const unsigned short Flags[] = {1};
	const unsigned char Areas[] = {0};
	dtNavMeshCreateParams Params{};
	Params.verts = Vertices;
	Params.vertCount = 4;
	Params.polys = Polygons;
	Params.polyFlags = Flags;
	Params.polyAreas = Areas;
	Params.polyCount = 1;
	Params.nvp = 4;
	Params.bmax[0] = 4.0f;
	Params.bmax[1] = 2.0f;
	Params.bmax[2] = 4.0f;
	Params.cs = 1.0f;
	Params.ch = 1.0f;
	Params.walkableHeight = 2.0f;
	Params.walkableRadius = 0.0f;
	Params.walkableClimb = 1.0f;
	Params.buildBvTree = true;

	unsigned char* Data = nullptr;
	int DataSize = 0;
	if (!TestTrue(TEXT("Create navmesh tile data"), dtCreateNavMeshData(&Params, &Data, &DataSize)))
	{
		return false;
	}

	dtNavMesh Mesh;
	const dtStatus MeshStatus = Mesh.init(Data, DataSize, DT_TILE_FREE_DATA);
	if (!TestTrue(TEXT("Initialize navmesh"), dtStatusSucceed(MeshStatus)))
	{
		dtFree(Data);
		return false;
	}
	dtNavMeshQuery Query;
	if (!TestTrue(TEXT("Initialize query"), dtStatusSucceed(Query.init(&Mesh, 64))))
	{
		return false;
	}

	const float Start[3] = {1.0f, 0.0f, 1.0f};
	const float End[3] = {3.0f, 0.0f, 3.0f};
	const float Extents[3] = {0.5f, 1.0f, 0.5f};
	const float Outside[3] = {10.0f, 0.0f, 10.0f};
	dtQueryFilter Filter;
	dtPolyRef StartRef = 0;
	dtPolyRef EndRef = 0;
	dtPolyRef OutsideRef = 0;
	TestTrue(TEXT("Find start polygon"), dtStatusSucceed(Query.findNearestPoly(Start, Extents, &Filter, &StartRef, nullptr)));
	TestTrue(TEXT("Find end polygon"), dtStatusSucceed(Query.findNearestPoly(End, Extents, &Filter, &EndRef, nullptr)));
	TestTrue(TEXT("Search outside mesh succeeds"), dtStatusSucceed(Query.findNearestPoly(Outside, Extents, &Filter, &OutsideRef, nullptr)));
	TestTrue(TEXT("Start polygon exists"), StartRef != 0);
	TestTrue(TEXT("End polygon exists"), EndRef != 0);
	TestTrue(TEXT("Outside search finds no polygon"), OutsideRef == 0);
	if (StartRef == 0 || EndRef == 0)
	{
		return false;
	}

	dtPolyRef Path[4] = {};
	int PathCount = 0;
	TestTrue(TEXT("Find path"), dtStatusSucceed(Query.findPath(StartRef, EndRef, Start, End, &Filter, Path, &PathCount, 4)));
	TestEqual(TEXT("Path contains one polygon"), PathCount, 1);
	if (PathCount > 0)
	{
		TestTrue(TEXT("Path contains the start polygon"), Path[0] == StartRef);
	}

	Filter.setExcludeFlags(1);
	dtPolyRef FilteredRef = 0;
	TestTrue(TEXT("Filtered search succeeds"), dtStatusSucceed(Query.findNearestPoly(Start, Extents, &Filter, &FilteredRef, nullptr)));
	TestTrue(TEXT("Excluded polygon is not returned"), FilteredRef == 0);
	return true;
}
