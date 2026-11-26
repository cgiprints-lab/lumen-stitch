# Lumen adapter contract v1

An adapter is everything Lumen needs to add Ansel-style photo capabilities
(free camera, time freeze, HUD-free capture, gigapixel / 360 / stereo jobs)
to one game. The manager app, the stitcher, and the tile math are shared and
game-agnostic; **the adapter supplies only three things**:

1. an **adapter manifest** (`adapters/<id>/adapter.json`) so the manager can
   detect the game and install the integration;
2. an **in-game integration** (native mod / plugin, engine-specific) that
   drives captures and writes the **job format** below;
3. a **CI artifact matrix** producing one integration bundle per declared
   platform (and version fallback policy).

Everything the game-side code emits is consumed by the shared stitcher. If
the job format and geometry conventions below hold, `lumen stitch` works
against your game with zero changes. The Dusklight adapter
(`adapters/dusklight/`) is the reference implementation of every rule here.

---

## 1. Adapter manifest — `adapter.json`

Parsed by the manager (`core/src/adapter.cpp`); unknown keys are ignored,
missing required keys make the adapter unloadable.

| key | required | meaning |
|---|---|---|
| `id` | yes | slug, matches the directory name |
| `name` | yes | display name |
| `detection.data_dirs` | yes | tokens for per-user install data dirs. Expansions: `%APPDATA%`, `%LOCALAPPDATA%`, `~` (home). One entry per OS family is fine; non-existent dirs are skipped |
| `detection.mod_subdir` | yes | subdirectory under the data dir where integration bundles live |
| `detection.markers` | yes | filenames that must exist for a dir to count as an install |
| `integration.mod_id` | yes | stable id of the in-game integration (used by the game's mod manager, uninstall bookkeeping) |
| `integration.bundle` | yes | bundle filename the installer copies into `mod_subdir` |
| `integration.source_dir` | no | source directory enabling `lumen install --build` |
| `integration.cmake_target` | no | CMake target for `--build` |
| `versions` | recommended | `verified`: game versions actually tested; `ci_matrix`: versions compiled against; `built_for`: version artifacts are built from; `fallback`: one-line policy statement |
| `artifacts` | yes | map platform slug → bundle path relative to the adapter dir. Slugs: `windows-x64`, `linux-x64`, `macos-arm64`, `android-arm64`. `lumen status`/`install` pick the current platform's slug; a missing file degrades to a clear "use --build or --artifact" error |

The manager keeps its own install records (id, timestamp, source) beside the
game data dir; install = copy bundle in, uninstall = remove it. Games whose
mod system hot-reloads allow live install/uninstall; otherwise document a
game restart in the adapter README.

## 2. Job format (the load-bearing contract)

A capture job = a set of **tile PNGs** plus **one JSON manifest** written by
the in-game integration into the user's chosen capture folder, on job
completion only (never mid-job: a partial job must not look stitchable).

### Files

- Tile images: flat names, no subdirectories (portable across host file
  APIs, incl. Android SAF-style grants): `<prefix>-<id>-r<row>-c<col>.png`
  for grids, `<prefix>-<id>-<n>.png` for ordered kinds — the names are
  carried verbatim in the manifest, so any flat name is legal. RGBA8 PNG.
- Manifest: `<prefix>-<id>.json` in the same folder; tile `file` entries are
  resolved relative to the manifest's directory.

### Manifest schema (`lumenJob: 1`)

```json
{
  "lumenJob": 1,
  "layout": "gigapixel" | "cube360" | "stereo",
  "scale": 4,
  "colorFormat": 2,
  "master": { "yaw": 1.1, "pitch": 0.3, "fovy": 90,
              "vw": 128, "vh": 96, "width": 512, "height": 384 },
  "tiles": [
    { "file": "lumen-1700-r0-c0.png", "row": 0, "col": 0,
      "yaw": 1.02, "pitch": 0.24, "fovy": 90,
      "width": 128, "height": 96,
      "eye": [0.0, 0.0, 0.0],
      "rect": [0, 0, 128, 96] }
  ]
}
```

| field | meaning |
|---|---|
| `layout` | which compositor the stitcher selects: `gigapixel` (shared-eye homography resample), `cube360` (equirectangular re-projection), `stereo` (1:1 side-by-side paste) |
| `scale` | informational: tiles per side (grid) or 1 for ordered kinds |
| `colorFormat` | numeric tag of the scene color target the tiles were read from (probe your host; Dusklight records its `GfxResolvedTargets.color_format`). The stitcher treats sRGB8 tiles as the floor; a float tag is the hook for a future HDR path |
| `master.yaw/pitch` | master camera pose, **radians**; `fovy` **degrees**, vertical |
| `master.vw/vh` | one capture viewport (the native tile size actually rendered) |
| `master.width/height` | master image extent the `rect`s live in (for grids: S·vw × S·vh, capped for memory; for stereo: the side-by-side extent) |
| `tile.yaw/pitch/fovy/width/height` | the sub-camera that tile was rendered with, same conventions |
| `tile.eye` | optional, defaults `[0,0,0]`: translation of that tile's eye relative to the master eye (the only layout that uses it is `stereo`) |
| `tile.rect` | pixel rectangle of the tile inside the master image: `[x0, y0, x1, y1]`, y-down, half-open |

### Geometry conventions (non-negotiable — both sides must agree)

- Right-handed, **Y-up**. Camera basis from yaw/pitch:
  - `right = (cos yaw, 0, −sin yaw)`
  - `forward = (sin yaw, 0, cos yaw)` rotated about `right` by pitch
  - `up = forward × right`
- Pinhole projection: pixel (i,j), width w, height h:
  `mx = ((i+0.5)/w·2 − 1)·tan(fovx/2)`, `my = (1 − (j+0.5)/h·2)·tan(fovy/2)`,
  `dir = forward + mx·right + my·up`, with `tan(fovx/2) = tan(fovy/2)·w/h`.
- **Shared-eye rule**: gigapixel and cube360 tiles are pure rotations of one
  eye (all `eye` zero). This is what makes the closed-form tile→master
  homography exact; an adapter that translates eyes in a "gigapixel" layout
  produces warped composites and fails the acceptance tests. Translations
  belong in `eye` under `stereo` semantics only.
- **FOV=90 rule**: grid seams align exactly when `fovy = 90°` and each tile
  keeps the viewport aspect. Ansel's panorama alignment test; the stitcher's
  `seam_test` enforces it.
- Use the shared math: `common/tilemath.h` (`basis`, `master_ray`,
  `tile_ray`, `cube360_faces`, `kCube360Fovy`) is included by both the game
  side and the stitcher — vendor it into your integration build so the two
  sides cannot drift.

### Layouts

- **gigapixel**: S×S grid of sub-frustums of the master frustum (projection
  offsets, never a viewport crop). `rect`s partition the master. Cap the
  master extent (reference: 65536 px/side) so strip-composited stitching
  stays within RAM.
- **cube360**: six FOV-90 faces — pitch 0 at yaw, yaw±90°, yaw±180°, plus
  pitch ±90° (slots from `cube360_faces`). The stitcher re-projects to a 2:1
  equirectangular panorama (`--equire N` sets width); face seams must land
  inside the natural overlaps of adjacent FOV-90 cones.
- **stereo**: three views (left/center/right) at the master FOV, eyes
  translated ±ipd/2 along the **master camera right axis**, recorded per
  tile in `eye`. The stitcher pastes 1:1 — with translated eyes no
  resampling is valid — so `rect`s must not overlap and `width` is the
  side-by-side extent.

### Capture-state rules (from the GDC 2017 Ansel contract)

- Freeze simulation while a session runs; keep rendering and your update
  loop alive; end the session on scene/level change (no softlocks).
- Capture **before HUD/UI compositing** so tiles are HUD-free without game
  edits.
- Settle latency: present N frames after any camera change before snapshot
  (configurable; default 2).
- During a multi-part job, hold off everything screen-space or temporal:
  vignette, motion blur, TAA, film grain, screen fades/bloom, letterboxes,
  and of course the HUD. Snapshot original state, re-assert per frame
  (a game-side write must not leak into a tile), restore at job end.
- Watchdogs everywhere: a capture whose readback never completes, a tile
  that fails outright, a viewport probe that stays unavailable must **end
  the job with an error** — never retry forever, never write a manifest
  over a partial tile set.

## 3. Feature floor and degradation

The integration probes its host APIs at load and gates each subsystem
independently (Dusklight: capture / freeze / auto-exit / consistency). The
floor is **1× HUD-free capture**: if it cannot be provided, photo mode must
stay off with a visible explanation, not half-run. Older game versions keep
whatever the floor allows. Log the exact feature set once per load.

## 4. Acceptance checklist for a new adapter

A new adapter is conformant when:

1. `seam_test`-equivalent: synthetic scene rendered through your tile
   pipeline vs. a reference render of the master camera agrees everywhere
   within ≤3/255 at FOV=90, 2×2 tiles, **including one-pixel seam flanks**;
   tile↔master ray round-trip closes < 1e-9 px. (Feed the same scene
   function to both sides — see `stitcher/test/seam_test.cpp`.)
2. cube360: six faces → equirectangular matches an exact reference over
   every panorama pixel (seam handling inside overlaps).
3. stereo: side-by-side matches a per-eye reference exactly (0 delta);
   `eye` offsets round-trip through your manifest writer + `load_job`.
4. BigTIFF output: tiled 256px, SubIFD mip chain, level-0 dimensions match
   the manifest master (or `--equire` width for panoramas).
5. One artifact built per platform slug declared in `adapter.json`; the
   version axis compiles clean across the game's supported line.
6. Degradation: with each optional host subsystem stubbed missing, the
   feature floor still works and no codepath hangs (watchdogs fire).

## 5. Wiring into the shared stack

- Stitcher consumption point: `stitcher/src/stitch.h` — `load_job()` parses
  the manifest (schema above); `composite` / `composite_scaled` /
  `composite_equire` / `composite_direct` are the layout compositors. If a
  future layout needs a new compositor, add it there and extend `layout`.
- CLI: `lumen stitch <job.json> [--downscale N] [--equire N] [--out ...]`.
  No adapter-specific flags — everything game-specific must already be in
  the manifest.
- Manager: adapters are loaded by scanning `adapters/*/adapter.json` next to
  the executable (or `--adapters <dir>`); add a slug + artifact path and the
  manager's install/status/detect flow works unchanged.

## 6. Porting map (what a new adapter is, concretely)

| piece | reuse as-is | write per game |
|---|---|---|
| `common/tilemath.h` | yes (vendor into the integration build) | — |
| Job manifest writer | schema | emit code (host file API) |
| Camera provider | conventions | host call (Dusklight: `register_camera_operator`) |
| Capture readback | state machine shape | host API (Dusklight: `resolve_pass` + WGSL compute copy) |
| Freeze / session | policy | host hooks |
| Effect stripping | list of what to strip | host symbols/cvars |
| HUD-free point | rule | host hook ordering |
| Manager detection | installer core | `adapter.json` detection block |
| Stitcher + tests | entirely | run the acceptance checklist |

The `_template` adapter directory contains the skeleton files (manifest,
integration outline, CMake sketch) to copy.
