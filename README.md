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
```

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
- **M1**: time freeze, free camera (camera operator), HUD-free 1x capture.
- **M2**: gigapixel tiled capture (2x-33x, projection-offset tiles) + BigTIFF
  stitcher.
- **M3**: effect overrides (vignette/temporal/LOD), EXR/HDR.
- **M4**: 360/stereo capture, per-platform artifacts, version fallback matrix.
- **M5**: adapter contract v1, second game adapter.

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
