# Recast and Detour API reference (vendored copy)

A working reference for code in this plugin that calls Recast or Detour directly. The signatures below are taken from the headers in `Public/Recast` and `Public/Detour` of **this** copy, which differs from upstream v1.6.0 in places (see `README.md`). When in doubt, read the header.

## Including

```cpp
// Build.cs: PrivateDependencyModuleNames.Add("RecastThirdParty");
THIRD_PARTY_INCLUDES_START
#include "Recast/Recast.h"
#include "Detour/DetourNavMesh.h"
#include "Detour/DetourNavMeshBuilder.h"
#include "Detour/DetourNavMeshQuery.h"
THIRD_PARTY_INCLUDES_END
```

- Do **not** call `rcAllocSetCustom` or `dtAllocSetCustom`, and do not replace the global log. They are process-wide and affect every user.
- Use a per-build `rcContext` subclass for logging instead (override `doLog`; timers are optional).
- The engine has its own Recast copy inside the `Navmesh` module. Never include both in one translation unit.

## Conventions

- **Space**: right-handed, **Y up**. Positions are `float[3]` = `{x, y, z}`; the ground plane is XZ. Grid "width" is X and "height/depth" is Z.
- **Winding**: a triangle faces up when its normal `cross(v1 - v0, v2 - v0)` has `y > 0`, which `rcClearUnwalkableTriangles` and `rcMarkWalkableTriangles` compute. With Y up this is counter-clockwise seen from above. Do not assume a winding for `rcPolyMesh` polygons: compute the signed area and normalize, as `CustomNavigationRecastBuilder.cpp` does.
- **Units**:

  | Unit | Meaning | Used for |
  |---|---|---|
  | wu | World units | Bounds, vertex positions, `cs`, `ch`, `detailSampleDist`, `detailSampleMaxError` |
  | vx | Voxels | `walkableHeight` and `walkableClimb` (in `ch`), `walkableRadius`, `maxEdgeLen` and `borderSize` (in `cs`) |

  `maxSimplificationError` is in cells; `minRegionArea` and `mergeRegionArea` are in cells (area).
- **Area ids**: `RC_NULL_AREA = 0` (not walkable), `RC_WALKABLE_AREA = 63`. Values 1..63 are user areas. Detour uses the same ids (`dtPoly::getArea`, at most 63).
- **Integer limits**:
  - `RC_SPAN_MAX_HEIGHT = 8191`: grid height in cells must stay below it.
  - `rcPolyMesh` indices are `unsigned short`; `RC_MESH_NULL_IDX = 0xffff` marks an unused polygon vertex.
  - At most 65 535 vertices per poly mesh.
  - `nvp` (verts per polygon) ≤ `DT_VERTS_PER_POLYGON = 6`.

## Recast build pipeline

Each step lists its call, the Recast objects it reads and writes, and notes:

1. **Grid size**:
   - Call: `rcCalcGridSize(const float* bmin, const float* bmax, float cs, int* w, int* h)`.
   - Gives `w = (int)((bmax[0]-bmin[0])/cs + 0.5f)`, and likewise for `h`.
2. **Heightfield**:
   - Call: `rcAllocHeightfield()`, then `rcCreateHeightfield(ctx, hf, w, h, bmin, bmax, cs, ch)`.
   - Free with `rcFreeHeightField` (capital F).
3. **Triangle areas**:
   - Call: `rcClearUnwalkableTriangles(ctx, slopeDeg, verts, nv, tris, nt, areas)`.
   - Reads the `areas` input; writes `areas`.
   - Sets steep triangles to `RC_NULL_AREA` and keeps the others' ids. `rcMarkWalkableTriangles` instead sets flat triangles to `RC_WALKABLE_AREA`.
4. **Rasterize**:
   - Call: `rcRasterizeTriangles(ctx, verts, nv, tris, areas, nt, hf, flagMergeThr)`.
   - Writes `hf` spans.
   - Overloads exist for `int` indices, `unsigned short` indices, or no indices (`verts` holds `nt*3` vertices).
   - `flagMergeThr` is usually `walkableClimb`.
   - Returns false on allocation failure.
5. **Filter**:
   - Calls: `rcFilterLowHangingWalkableObstacles(ctx, walkableClimb, hf)`, `rcFilterLedgeSpans(ctx, walkableHeight, walkableClimb, hf)`, `rcFilterWalkableLowHeightSpans(ctx, walkableHeight, hf)`.
   - Edit `hf` in place.
   - Each one is optional.
6. **Compact**:
   - Call: `rcAllocCompactHeightfield()`, then `rcBuildCompactHeightfield(ctx, walkableHeight, walkableClimb, hf, chf)`.
   - Reads `hf`; writes `chf`.
   - `chf.spanCount == 0` means nothing walkable. The heightfield may be freed after this.
7. **Erode**:
   - Call: `rcErodeWalkableArea(ctx, walkableRadius, chf)`.
   - Edits `chf` areas.
   - Skip it when the radius is 0.
8. **Mark areas** (optional):
   - Calls: `rcMarkBoxArea`, `rcMarkConvexPolyArea(ctx, verts, nverts, minY, maxY, area, chf)`, `rcMarkCylinderArea`.
   - Edit `chf` areas.
9. **Partition**, one of:
   - Watershed: `rcBuildDistanceField(ctx, chf)`, then `rcBuildRegions(ctx, chf, borderSize, minArea, mergeArea)`. Best quality and slowest.
   - Monotone: `rcBuildRegionsMonotone(ctx, chf, borderSize, minArea, mergeArea)`. Fast, but gives long thin polygons.
   - Layers: `rcBuildLayerRegions(ctx, chf, borderSize, minArea)`.

   Writes `chf` regions. `borderSize` is 0 for a single tile and `walkableRadius + 3` for tiles.
10. **Contours**:
    - Call: `rcAllocContourSet()`, then `rcBuildContours(ctx, chf, maxError, maxEdgeLen, cset, RC_CONTOUR_TESS_WALL_EDGES)`.
    - Reads `chf`; writes `cset`.
    - `cset.nconts == 0` means no regions.
11. **Poly mesh**:
    - Call: `rcAllocPolyMesh()`, then `rcBuildPolyMesh(ctx, cset, nvp, pmesh)`.
    - Reads `cset`; writes `pmesh`.
12. **Detail mesh**:
    - Call: `rcAllocPolyMeshDetail()`, then `rcBuildPolyMeshDetail(ctx, pmesh, chf, sampleDist, sampleMaxError, dmesh)`.
    - Reads `pmesh` and `chf`; writes `dmesh`.
    - `sampleDist < 0.9*cs` disables sampling.

Each `rcAlloc*` has a matching `rcFree*`. They return `nullptr` on out-of-memory. Every `rcBuild*` returns `false` on failure and logs through `ctx`. Use RAII in Unreal code (see `TRecastPtr` in `CustomNavigationRecastBuilder.cpp`).

### Key structures

`rcPolyMesh`:
- `verts`: `unsigned short[nverts*3]`, grid coordinates. World position is `bmin + (v[0]*cs, v[1]*ch, v[2]*cs)`.
- `polys`: `unsigned short[npolys*2*nvp]`. The first `nvp` entries are vertex indices, padded with `RC_MESH_NULL_IDX`. The next `nvp` entries are neighbours: a polygon index, `RC_MESH_NULL_IDX` for a wall, or the `0x8000` bit for a tile border.
- `regs`, `flags` (`unsigned short`) and `areas` (`unsigned char`), one per polygon.
- Also: `nvp`, `bmin`, `bmax`, `cs`, `ch`, `borderSize`, `maxEdgeError`.

`rcPolyMeshDetail`:
- `meshes`: `unsigned int[nmeshes*4]`, one entry per polygon: `{vertBase, vertCount, triBase, triCount}`.
- `verts`: `float[nverts*3]`, world units, absolute.
- `tris`: `unsigned char[ntris*4]`. The first three entries are indices **relative to that polygon's `vertBase`**; the fourth holds edge flags.

`rcCompactHeightfield`:
- `width`, `height`, `spanCount`, `walkableHeight`, `walkableClimb`, `borderSize`, `maxRegions`.
- Arrays: `cells`, `spans`, `dist`, `areas`.

`rcConfig`: a plain struct holding all the parameters above. It is optional; the functions take the values directly.

## Detour (runtime navmesh and queries)

Detour is not used by `CustomNavigationUnreal` yet. Typical single-tile setup:

```cpp
// 1. Fill the create params from rcPolyMesh + rcPolyMeshDetail (vertices in Recast space).
dtNavMeshCreateParams P = {};
P.verts = pmesh.verts; P.vertCount = pmesh.nverts;
P.polys = pmesh.polys; P.polyAreas = pmesh.areas; P.polyFlags = flags /* non-zero, else polys are filtered out */;
P.polyCount = pmesh.npolys; P.nvp = pmesh.nvp;
P.detailMeshes = dmesh.meshes; P.detailVerts = dmesh.verts; P.detailVertsCount = dmesh.nverts;
P.detailTris = dmesh.tris; P.detailTriCount = dmesh.ntris;
P.walkableHeight = AgentHeightWu; P.walkableRadius = AgentRadiusWu; P.walkableClimb = AgentClimbWu; // world units here
rcVcopy(P.bmin, pmesh.bmin); rcVcopy(P.bmax, pmesh.bmax);
P.cs = cs; P.ch = ch; P.buildBvTree = true;

unsigned char* Data = nullptr; int DataSize = 0;
dtCreateNavMeshData(&P, &Data, &DataSize);                    // false on bad input

// 2. Navmesh owns Data with DT_TILE_FREE_DATA.
dtNavMesh* Nav = dtAllocNavMesh();
Nav->init(Data, DataSize, DT_TILE_FREE_DATA);                 // single-tile; tiled: init(const dtNavMeshParams*) + addTile(...)

// 3. Query.
dtNavMeshQuery* Q = dtAllocNavMeshQuery();
Q->init(Nav, 2048 /*maxNodes*/);
dtQueryFilter Filter;                                         // include/exclude flags, per-area costs
dtPolyRef StartRef; float StartPt[3];
Q->findNearestPoly(StartPos, HalfExtents, &Filter, &StartRef, StartPt);
Q->findPath(StartRef, EndRef, StartPt, EndPt, &Filter, PathRefs, &PathCount, MaxPath);
Q->findStraightPath(StartPt, EndPt, PathRefs, PathCount, StraightPts, StraightFlags, StraightRefs, &StraightCount, MaxStraight);

dtFreeNavMeshQuery(Q); dtFreeNavMesh(Nav);
```

Other query calls (in `DetourNavMeshQuery.h`): `raycast`, `moveAlongSurface`, `findRandomPoint`, `closestPointOnPoly`, `getPolyHeight`, `findPolysAroundCircle` and `queryPolygons`.

- **Status**: every call returns `dtStatus`. Check it with `dtStatusSucceed(s)`, `dtStatusFailed(s)`, `dtStatusInProgress(s)` and `dtStatusDetail(s, DT_BUFFER_TOO_SMALL)`, among others.
- **Threading**: a `dtNavMesh` may be read concurrently, but each thread needs its own `dtNavMeshQuery`. Mutating tiles (`addTile` / `removeTile`) needs exclusive access.

## Unit conversion used by this plugin

Unreal cm, Z up, to Recast (see the plugin README):
- Position: `(X, Z, Y) - origin`.
- Triangle `(A, B, C)` becomes `(A, C, B)`.
- `walkableHeight = ceil(H/ch)`, `walkableClimb = floor(C/ch)`, `walkableRadius = ceil(R/cs)`, `maxEdgeLen = floor(L/cs)`.
- Check that `walkableHeight ≥ 3` and `ceil((bmax.y - bmin.y)/ch) ≤ 8191`.
