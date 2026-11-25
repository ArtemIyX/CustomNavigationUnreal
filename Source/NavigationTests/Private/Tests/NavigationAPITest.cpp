//Created by Stalker7274

#include "Misc/AutomationTest.h"
#include "Recast.h"
#include "RecastAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourNavMeshBuilder.h"
#include "DetourCommon.h"
#include "DetourAlloc.h"
#include "DetourAssert.h"

DEFINE_LOG_CATEGORY_STATIC(LogRecastAPITest, Log, All);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNavigationAPITest,
	"CustomNavigation.API",
	EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::EngineFilter
)

bool FNavigationAPITest::RunTest(const FString& Parameters)
{
	UE_LOG(LogRecastAPITest, Warning, TEXT("=== RecastThirdParty API Link + Runtime Test START ==="));

	// Allocators
	auto p_rcAlloc              = &rcAlloc;
	auto p_rcFree               = &rcFree;
	auto p_rcAllocSetCustom     = &rcAllocSetCustom;
	auto p_dtAlloc              = &dtAlloc;
	auto p_dtFree               = &dtFree;
	auto p_dtAllocSetCustom     = &dtAllocSetCustom;

	// Assert
	auto p_rcAssertFailSetCustom = &rcAssertFailSetCustom;
	auto p_dtAssertFailSetCustom = &dtAssertFailSetCustom;
	auto p_dtAssertFailGetCustom = &dtAssertFailGetCustom;

	// Heightfield / Compact / Contours / PolyMesh
	auto p_rcAllocHeightfield           = &rcAllocHeightfield;
	auto p_rcFreeHeightField            = &rcFreeHeightField;
	auto p_rcCreateHeightfield          = &rcCreateHeightfield;
	auto p_rcAllocCompactHeightfield    = &rcAllocCompactHeightfield;
	auto p_rcFreeCompactHeightfield     = &rcFreeCompactHeightfield;
	auto p_rcBuildCompactHeightfield    = &rcBuildCompactHeightfield;
	auto p_rcAllocContourSet            = &rcAllocContourSet;
	auto p_rcFreeContourSet             = &rcFreeContourSet;
	auto p_rcBuildContours              = &rcBuildContours;
	auto p_rcAllocPolyMesh              = &rcAllocPolyMesh;
	auto p_rcFreePolyMesh               = &rcFreePolyMesh;
	auto p_rcBuildPolyMesh              = &rcBuildPolyMesh;
	auto p_rcAllocPolyMeshDetail        = &rcAllocPolyMeshDetail;
	auto p_rcFreePolyMeshDetail         = &rcFreePolyMeshDetail;
	auto p_rcBuildPolyMeshDetail        = &rcBuildPolyMeshDetail;
	auto p_rcAllocHeightfieldLayerSet   = &rcAllocHeightfieldLayerSet;
	auto p_rcFreeHeightfieldLayerSet    = &rcFreeHeightfieldLayerSet;
	auto p_rcBuildHeightfieldLayers     = &rcBuildHeightfieldLayers;

	// Rasterization / Filtering / Areas
	auto p_rcRasterizeTriangle          = &rcRasterizeTriangle;
	bool (*p_rcRasterizeTriangles)( rcContext*, const float*, const int, const int*,
	const unsigned char*, const int, rcHeightfield&, const int) = &rcRasterizeTriangles;
	auto p_rcFilterLowHangingWalkableObstacles = &rcFilterLowHangingWalkableObstacles;
	auto p_rcFilterLedgeSpans           = &rcFilterLedgeSpans;
	auto p_rcFilterWalkableLowHeightSpans = &rcFilterWalkableLowHeightSpans;
	auto p_rcErodeWalkableArea          = &rcErodeWalkableArea;
	auto p_rcMedianFilterWalkableArea   = &rcMedianFilterWalkableArea;
	auto p_rcMarkWalkableTriangles      = &rcMarkWalkableTriangles;
	auto p_rcClearUnwalkableTriangles   = &rcClearUnwalkableTriangles;
	auto p_rcMarkBoxArea                = &rcMarkBoxArea;
	auto p_rcMarkConvexPolyArea         = &rcMarkConvexPolyArea;
	auto p_rcMarkCylinderArea           = &rcMarkCylinderArea;

	// Regions / Distance field
	auto p_rcBuildDistanceField         = &rcBuildDistanceField;
	auto p_rcBuildRegions               = &rcBuildRegions;
	auto p_rcBuildRegionsMonotone       = &rcBuildRegionsMonotone;
	auto p_rcBuildLayerRegions          = &rcBuildLayerRegions;

	// Merge / Copy
	auto p_rcCopyPolyMesh               = &rcCopyPolyMesh;
	auto p_rcMergePolyMeshes            = &rcMergePolyMeshes;
	auto p_rcMergePolyMeshDetails       = &rcMergePolyMeshDetails;

	// Utility
	auto p_rcCalcBounds                 = &rcCalcBounds;
	auto p_rcCalcGridSize               = &rcCalcGridSize;

	// Detour NavMesh
	auto p_dtAllocNavMesh               = &dtAllocNavMesh;
	auto p_dtFreeNavMesh                = &dtFreeNavMesh;
	auto p_dtCreateNavMeshData          = &dtCreateNavMeshData;
	auto p_dtNavMeshHeaderSwapEndian    = &dtNavMeshHeaderSwapEndian;
	auto p_dtNavMeshDataSwapEndian      = &dtNavMeshDataSwapEndian;

	// Detour Query
	auto p_dtAllocNavMeshQuery          = &dtAllocNavMeshQuery;
	auto p_dtFreeNavMeshQuery           = &dtFreeNavMeshQuery;

	// Detour Common (не-inline)
	auto p_dtClosestPtPointTriangle     = &dtClosestPtPointTriangle;
	auto p_dtClosestHeightPointTriangle = &dtClosestHeightPointTriangle;
	auto p_dtIntersectSegmentPoly2D     = &dtIntersectSegmentPoly2D;
	auto p_dtIntersectSegSeg2D          = &dtIntersectSegSeg2D;
	auto p_dtPointInPolygon             = &dtPointInPolygon;
	auto p_dtDistancePtPolyEdgesSqr     = &dtDistancePtPolyEdgesSqr;
	auto p_dtCalcPolyCenter             = &dtCalcPolyCenter;
	auto p_dtOverlapPolyPoly2D          = &dtOverlapPolyPoly2D;
	auto p_dtRandomPointInConvexPoly    = &dtRandomPointInConvexPoly;

	// Чтобы компилятор точно не выкинул указатели
	volatile uintptr_t ForceLink =
	(uintptr_t)p_rcAlloc +
	(uintptr_t)p_dtAlloc +
	(uintptr_t)p_rcAllocHeightfield +
	(uintptr_t)p_dtAllocNavMesh +
	(uintptr_t)p_dtAllocNavMeshQuery +
	(uintptr_t)p_rcBuildCompactHeightfield +
	(uintptr_t)p_rcBuildPolyMesh +
	(uintptr_t)p_dtCreateNavMeshData +
	(uintptr_t)p_dtClosestPtPointTriangle;

	(void)ForceLink;

	UE_LOG(LogRecastAPITest, Log, TEXT("Link-time symbols resolved OK"));

	bool bAllOk = true;

	// --- Allocators ---
	{
		void* ptr = rcAlloc(64, RC_ALLOC_PERM);
		if (!ptr)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAlloc failed"));
			bAllOk = false;
		}
		else
		{
			rcFree(ptr);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAlloc / rcFree OK"));
		}

		ptr = dtAlloc(64, DT_ALLOC_PERM);
		if (!ptr)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("dtAlloc failed"));
			bAllOk = false;
		}
		else
		{
			dtFree(ptr);
			UE_LOG(LogRecastAPITest, Log, TEXT("dtAlloc / dtFree OK"));
		}
	}

	// --- Context ---
	{
		rcContext* ctx = new rcContext(false);
		if (!ctx)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcContext allocation failed"));
			bAllOk = false;
		}
		else
		{
			delete ctx;
			UE_LOG(LogRecastAPITest, Log, TEXT("rcContext OK"));
		}
	}

	// --- Heightfield ---
	{
		rcHeightfield* hf = rcAllocHeightfield();
		if (!hf)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocHeightfield failed"));
			bAllOk = false;
		}
		else
		{
			rcFreeHeightField(hf);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocHeightfield / rcFreeHeightField OK"));
		}
	}

	// --- CompactHeightfield ---
	{
		rcCompactHeightfield* chf = rcAllocCompactHeightfield();
		if (!chf)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocCompactHeightfield failed"));
			bAllOk = false;
		}
		else
		{
			rcFreeCompactHeightfield(chf);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocCompactHeightfield / rcFreeCompactHeightfield OK"));
		}
	}

	// --- ContourSet ---
	{
		rcContourSet* cset = rcAllocContourSet();
		if (!cset)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocContourSet failed"));
			bAllOk = false;
		}
		else
		{
			rcFreeContourSet(cset);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocContourSet / rcFreeContourSet OK"));
		}
	}

	// --- PolyMesh ---
	{
		rcPolyMesh* mesh = rcAllocPolyMesh();
		if (!mesh)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocPolyMesh failed"));
			bAllOk = false;
		}
		else
		{
			rcFreePolyMesh(mesh);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocPolyMesh / rcFreePolyMesh OK"));
		}
	}

	// --- PolyMeshDetail ---
	{
		rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
		if (!dmesh)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocPolyMeshDetail failed"));
			bAllOk = false;
		}
		else
		{
			rcFreePolyMeshDetail(dmesh);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocPolyMeshDetail / rcFreePolyMeshDetail OK"));
		}
	}

	// --- HeightfieldLayerSet ---
	{
		rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
		if (!lset)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("rcAllocHeightfieldLayerSet failed"));
			bAllOk = false;
		}
		else
		{
			rcFreeHeightfieldLayerSet(lset);
			UE_LOG(LogRecastAPITest, Log, TEXT("rcAllocHeightfieldLayerSet / rcFreeHeightfieldLayerSet OK"));
		}
	}

	// --- Detour NavMesh ---
	{
		dtNavMesh* nav = dtAllocNavMesh();
		if (!nav)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("dtAllocNavMesh failed"));
			bAllOk = false;
		}
		else
		{
			dtFreeNavMesh(nav);
			UE_LOG(LogRecastAPITest, Log, TEXT("dtAllocNavMesh / dtFreeNavMesh OK"));
		}
	}

	// --- Detour NavMeshQuery ---
	{
		dtNavMeshQuery* query = dtAllocNavMeshQuery();
		if (!query)
		{
			UE_LOG(LogRecastAPITest, Error, TEXT("dtAllocNavMeshQuery failed"));
			bAllOk = false;
		}
		else
		{
			dtFreeNavMeshQuery(query);
			UE_LOG(LogRecastAPITest, Log, TEXT("dtAllocNavMeshQuery / dtFreeNavMeshQuery OK"));
		}
	}

	{
		float a[3] = { 0,0,0 };
		float b[3] = { 1,0,0 };
		float c[3] = { 0,0,1 };
		float closest[3];
		dtClosestPtPointTriangle(closest, a, a, b, c);
		UE_LOG(LogRecastAPITest, Log, TEXT("dtClosestPtPointTriangle OK"));
	}

	if (bAllOk)
	{
		UE_LOG(LogRecastAPITest, Log, TEXT("=== ALL RecastThirdParty API tests PASSED ==="));
	}
	else
	{
		UE_LOG(LogRecastAPITest, Error, TEXT("=== SOME RecastThirdParty API tests FAILED ==="));
	}

	return bAllOk;
}