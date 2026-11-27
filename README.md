# Lumen

An NVIDIA Ansel-style photo tool for decompiled games. Lumen applies a **free
camera, time freeze, HUD-free capture and gigapixel ("super resolution")
screenshots** to supported games **without modifying them**: it installs an
integration (a game-specific mod bundle) into the user's game install.

First adapter: [Dusklight](https://github.com/TwilitRealm/dusklight) (a
reimplementation of *The Legend of Zelda: Twilight Princess*). The integration
is a native `.dusk` mod built out-of-tree against the Dusklight mod SDK;
Dusklight itself is never forked or patched.

## Repository layout

```
core/        Lumen Manager - standalone CLI that detects installs and
             installs/uninstall adapter integration bundles
adapters/    one directory per supported game (adapter manifest + integration)
  dusklight/ the Dusklight adapter: adapter.json + mod/ (native .dusk mod)
stitcher/    gigapixel stitcher (tile manifest -> tiled BigTIFF/PNG + mips)
docs/        adapter contract and design notes
```

## Quick start (M0)

```
# build the manager and the dusklight adapter mod
cmake -S core -B build/core -G Ninja
cmake --build build/core

cmake -S adapters/dusklight/mod -B build/dusklight-mod -G Ninja
cmake --build build/dusklight-mod          # produces build/dusklight-mod/mods/lumen_photo.dusk

# apply / inspect / remove the integration on a stock Dusklight install
build/core/lumen status
build/core/lumen install --adapter dusklight --artifact build/dusklight-mod/mods/lumen_photo.dusk
build/core/lumen uninstall --adapter dusklight

# stitch a gigapixel job captured in-game: tiles, manifest, and the stitched
# BigTIFF + previews land in the capture folder automatically
# (Pictures/Lumen by default); offline re-stitch of any job:
build/core/lumen stitch "path/to/capture-folder/lumen-<id>.json" --out panorama.tiff
```

In-game hotkeys (photo mode off = plain game, every game key untouched):

| Key           | Action                                            |
|---------------|---------------------------------------------------|
| F1            | enter/exit photo mode (free camera + time freeze)  |
| F7            | single screenshot (HUD-free)                       |
| Shift+F7      | stereo trio -> side-by-side image                  |
| F12           | gigapixel super-resolution (default 8x8 tiles)     |
| Shift+F12     | 360 panorama (six faces -> equirectangular)        |
| Alt+F12       | gigapixel variant that can't collide with Steam    |

F2-F6 and F8-F11 stay the game's own keys (F11 is Dusklight's fullscreen
toggle). WASD + arrow keys fly the camera; mouse look stays available.

Dusklight mod dirs used by the installer:

| OS      | mods directory |
|---------|----------------|
| Windows | `%APPDATA%\TwilitRealm\Dusklight\mods` |
| Linux   | `~/.local/share/TwilitRealm/Dusklight/mods` |
| macOS   | `~/Library/Application Support/TwilitRealm/Dusklight/mods` |

Mods hot-reload in Dusklight, so install/uninstall take effect without a
restart.

## Feature status

- **M0** (done): manager CLI (status/install/uninstall), adapter manifest, mod
  skeleton (in-game mod panel, session toggle toast, settings persistence).
- **M1** (done): time freeze, free camera (camera operator), HUD-free 1x
  capture (F7, WASD/arrow flight).
- **M2** (done): gigapixel capture — F12 renders an S×S grid of projection-offset sub-frustums at one frozen eye
  (2–33 tiles per side, capped so the master image stays within 65536 px per
  side), tiles stream to the capture folder with a JSON manifest;
  `lumen stitch` composites them strip-by-strip into a tiled
  BigTIFF with a SubIFD mip chain plus JPEG/PNG previews, and `--downscale N`
  produces a true box downsample of the master grid directly. Shared tile math
  (`common/tilemath.h`) is exercised by `stitcher/test/seam_test.cpp`
  (FOV=90 seam fidelity vs a reference render, BigTIFF structure, downsample
  fidelity).
- **M3** (done): gigapixel consistency
  — while a job runs, viewport-relative and temporal effects are held off
  (cloud-shadow haze draw skipped; `mDoGph_gInf_c` bloom/fade/motion-blur
  state pinned off, snapshot + restored on job end), each with a panel toggle
  and graceful per-symbol degrade. Scene color format is probed and recorded
  in the manifest; Dusklight's scene target is 8-bit RGBA, so HDR/EXR output
  is wired at the manifest level for future adapters but produces no gain on
  this one (documented in the mod log if a float format ever appears).
- **M4** (done): 360 + stereo +
  hardening — Shift+F12 captures six axis-aligned cube faces (poles at 87.5°
  with a 95° vertical FOV so they cover the poles without hitting the
  lookAt-degenerate axis) at the frozen
  eye (`lumen stitch` composes an equirectangular panorama, `--equire N` sets
  its width; seams land inside face overlaps); Shift+F7 captures a stereo trio
  (left/center/right eyes shifted along the camera right axis, panel-adjustable
  separation; stitched 1:1 side-by-side — translated eyes make resampling
  wrong, so the paste is exact and per-eye `eye` offsets ride the manifest for
  future redressers). Capture jobs are watchdogged (stalled readback, failing
  tile, unreachable viewport probe all end the job with an error instead of
  hanging or spamming); a CI matrix builds the tools on Windows/Linux/macOS
  and the mod matrix compiles across Dusklight v2.0.1–v2.0.3 per platform —
  at runtime every subsystem (capture/freeze/auto-exit/consistency) gates
  independently, so an older Dusklight keeps the 1× capture floor instead of
  half-running.
- **M5** (done): adapter contract v1 — `docs/adapter-contract.md` specifies
  the three pieces every game adapter supplies (adapter.json schema, the
  `lumenJob` tile/manifest job format with its geometry conventions, and the
  CI artifact matrix), plus capture-state rules, the feature-degradation
  floor, and the acceptance checklist; `adapters/_template/` carries the
  copyable skeleton (manifest, integration outline, CMake shape). The
  stitcher and manager need zero changes for a conformant new game.
- **M6** (done): end-to-end in one keypress — completed capture jobs are
  stitched **in-mod** on a background worker (exception-proofed, one retry
  under memory pressure, graceful "parts saved" toast on failure; `lumen
  stitch <manifest>` recovers any interrupted job); discoverability (entry
  toast, in-mode cheat sheet, error toasts for every rejected job); color
  readback matched against the live display; 360 poles kept off the lookAt
  degenerate axis; camera pose restored exactly on exit.

See `docs/adapter-contract.md` for how to add a new game.

## Notes

- **Toolchains**: MSVC (CI matrix) and clang-cl are the supported Windows
  compilers for the Dusklight integration — its mod metadata lives in MSVC PE
  sections (`modmeta$a$d$z`). MinGW compiles the mod only as a local
  compile-check (see the shim in `mod/src/mod.cpp`); MinGW bundles are not
  loadable. Linux/macOS builds use native ELF/Mach-O sections normally.
- NVIDIA's original Ansel SDK (UE/Unity) repos are gone from GitHub; the
  integration contract implemented here is rebuilt from NVIDIA's public GDC
  2017 material on Ansel (session model, capture/settle latency,
  projection-offset super resolution, the FOV=90 panorama alignment test).
- Dusklight is CC0-1.0. Lumen ships no game assets; users provide their own
  game dump.
- **Lumen itself is CC0-1.0** (see `LICENSE`): the manager, stitcher, adapter
  and mod are public domain — free to modify, recompile and redistribute,
  with no attribution required.
