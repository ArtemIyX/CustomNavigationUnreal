# Recast Navigation

- Upstream: https://github.com/recastnavigation/recastnavigation
- Intended reference release: [v1.6.0](https://github.com/recastnavigation/recastnavigation/releases/tag/v1.6.0)
- Release commit: `6dc1667f580357e8a2154c28b7867bea7e8ad3a7`
- Included components: Recast and Detour. DetourCrowd, DetourTileCache, DebugUtils and RecastDemo are not included.
- License: see `License.txt` in this directory and the notices in the source files.

For calling Recast and Detour from plugin code (pipeline, signatures, units, Detour setup) see [RECAST_API.md](RECAST_API.md). For the plugin API see the [plugin README](../../README.md).

## Source provenance and local changes

The intended reference is v1.6.0, but the current files are **not an exact copy of that release**. A comparison against the release found differences beyond Unreal integration, including changes in Recast area processing and Detour navigation code. The exact origin of those differences has not been established; do not treat the release commit as a verified pin for the current source.

The headers add `RECASTTHIRDPARTY_API` annotations to expose declarations through the Unreal module. Source files also have changes such as include ordering. The altered files have not yet been fully audited or individually marked; compare them against the reference before replacing or redistributing them, and preserve the upstream copyright and license notices.

The module is a regular Unreal runtime module. UBT compiles the implementation files under `Private/Recast` and `Private/Detour`; no separate CMake build is required. Unity builds are disabled for this module. `NavigationTests` contains the `CustomNavigation.API` editor automation smoke test.

## Updating

1. Check out the desired upstream release or commit and record its full commit hash and URL.
2. Compare the existing Recast and Detour files with both the current reference and the new revision. Identify and document all local differences before deciding which to retain.
3. Replace the included headers and implementation files, preserving upstream notices and the license. Reapply necessary Unreal integration changes and clearly mark altered upstream files.
4. Update this document with the verified revision, included components and complete local modification inventory.
5. Build the available Unreal Editor target with UBT and run `CustomNavigation.API`. Verify coexistence with Unreal's navigation modules and record the engine version, commands, results and tested platforms.
