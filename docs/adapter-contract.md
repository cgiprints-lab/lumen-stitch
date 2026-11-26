# Lumen adapter contract

An adapter is a directory under `adapters/<id>/` containing:

```
adapters/<id>/
├─ adapter.json        manifest: detection, integration identity, artifacts
├─ mod/ (or similar)   the integration, built out-of-tree against the game's
│                      own plugin/mod SDK - the game is never forked or patched
└─ artifacts/          optional prebuilt per-platform bundles (CI output)
```

## adapter.json

| key | meaning |
|-----|---------|
| `id`, `name` | adapter identity |
| `detection.data_dirs` | path tokens (`~`, `%VAR%`, `$VAR`) where the game may keep its user data |
| `detection.mod_subdir` | subdirectory receiving integration bundles |
| `detection.markers` | any-of files identifying a real install |
| `integration.mod_id` | identity of the integration inside the game's mod system |
| `integration.bundle` | bundle filename installed into `mod_subdir` |
| `integration.source_dir`, `cmake_target` | for `lumen install --build` |
| `artifacts.<platform>` | prebuilt bundle path; slugs: `windows-x64`, `linux-x64`, `macos-arm64`, `android-arm64` |

## Roles an integration must implement

Each engine differs, but every adapter maps these roles onto its game's APIs
(the Dusklight adapter is the reference implementation):

1. **Session controller** - a hotkey/trigger that starts/stops a photo session
   and freezes or approves the game loop (Ansel's start/stop callbacks).
   Sessions must end cleanly on stage/menu transitions.
2. **Camera provider/override** - read the live camera; override it per frame
   with the free camera; expose enough projection detail (FOV in degrees,
   framebuffer aspect, clip/depth conventions) that tile projections can be
   derived exactly like the base camera.
3. **Capture hook** - a mid-frame capture point with and without HUD, plus
   offscreen snapshot + readback (and compute if the engine supports it).
4. **Scene overrides** - toggles to disable non-uniform post effects (vignette),
   temporal effects (motion blur, TAA) and LOD reductions during multipart
   capture; a configurable settle-frame count.
5. **UI + settings** - in-game panel for options and progress, persisted config.
6. **Install target** - where bundles live and how they hot-reload.

Engine-agnostic pieces live in `core/` (detection/install) and `stitcher/`
(tiling output assembly). The capture math (tile projections, settle state
machine) is intended to be shared source between adapters.

## Validation

An adapter is accepted when it passes the alignment test: FOV 90°, a full 360°
yaw panorama of 4+ tiles lines up seamlessly, and a 2x/4x super-resolution
shot shows no per-tile seams or exposure drift.
