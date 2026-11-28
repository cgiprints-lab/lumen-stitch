# Lumen adapter: Longshot

Ansel-style photo capabilities for [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)
(the static recompilation of the Switch OoT build): **free camera, world
freeze, HUD-free capture, gigapixel image stitching, 360 cube, and a stereo
trio** - the full Lumen Stitch feature set, as a separate out-of-tree mod.

Nothing in Longshot is forked or patched: this is one plain manifest mod
(TypeScript, the Modding API) plus a small capture-seam proposal upstream.
Longshot developers keep pushing updates independently; the mod is written
against the stable API (`api = "0.1"`) and feature-probes each subsystem.

## Install

From a Lumen checkout, with the manager built:

```bash
lumen install --adapter longshot --build
```

The manager detects the game folder (the folder with `image.bin`; by default
`%USERPROFILE%\Games\Longshot` on Windows, `~/Games/Longshot` on Linux) and
copies the bundle folder into that profile's `mods/` (`<game>/profile/mods/`).
If you play through **RecompLauncher** (`%APPDATA%\Longshot`), copy the built
`mods/lumen-photo` folder into that profile's `mods/` yourself.

Then **restart the game** (Longshot indexes mods only at start), turn
**Lumen Photo Suite** on in the Insert menu's Mods tab, and accept its code
and permissions when asked - those prompts are yours to answer in the game.
After that, the mod is a folder mod in the profile's `mods/`, so editing its
files hot-reloads it by itself (docs/modding/dev.md) - a `mod.toml` change
waits for the next start.

## Controls (controller-first, all remappable in the mod's settings)

| Action | Controller | Keyboard |
|---|---|---|
| Photo mode on / off | **L3 + R3** together | **F8** |
| Look | right stick / mouse | - |
| Fly | left stick | - |
| Up / down | ZL / ZR | - |
| Choose job: single → stereo → gigapixel → 360 | **R** (outside), **D-pad ◀ ▶** (in mode) | F5 / dpad |
| Take the shot (runs the chosen job) | **ZR** (outside), **A** (in mode) | F6 |
| Cancel a running job | **B** | - |

Notes: L3 is free in Longshot (sprint was retuned upstream, issue #2038);
F1 is Longshot's own menu key and F12 Steam's, so neither is used; if you
enable the interface mod's command-palette key, keep it off F5/F6/F8.

Settings: flight speed, gigapixel tiles per side (2-33, capped so the master
stays under 65 536 px/side), stereo eye separation, settle frames before
each readback (temporal pipelines), freeze on/off, HUD-hide on/off.

## Capture and the capture seam

Capture (reading the scene back to PNG) **is not in the Modding API yet**:
`game/modapi/schema/gfx.toml` says plainly *"nothing reads its output back"*.
The mod therefore runs in two tiers:

- **Photo mode (works today, API 0.1):** free camera, `game.pause()` freeze
  (Link, enemies, effects and timers all stop - the game's own pause manager),
  `hideGameHud()` for HUD-free framing, and job planning. Pressing capture
  logs the job's exact plan (and keeps it in the mod's profile storage) and
  says the seam is missing; nothing is written.
- **Capture (when the seam lands, see [seams.md](seams.md)):** the same
  button drives the real thing: per-tile `capture.frame()` readbacks through
  the frozen, HUD-free, overlay-held view, and a `lumenJob:1` manifest in
  the game's `captures/lumen/` folder.

Stitching is the shared Lumen stitcher, zero game-specific flags:

```bash
lumen stitch "<game folder>/captures/lumen/lumen-<id>.json" --out photo.tiff
```

The gigapixel output is a tiled BigTIFF with a SubIFD mip chain (gigapixel
viewing); cube jobs re-project to an equirectangular panorama (`--equire`),
stereo pastes side-by-side.

## Layout

```
adapter.json            detection + artifact matrix (manager reads it)
mod/                    the manifest mod itself (mod.toml, src/, test/)
  src/main.ts           photo mode, the pad scheme, the capture state machine
  src/plan.ts           job planning: manifests for gigapixel / cube360 / stereo
  src/tilemath.ts       TypeScript mirror of ../../common/tilemath.h
  test/                 fake-game tests (`Longshot.exe mod test`)
tests/                  tile-math equivalence: C++ fixture + equiv_test.ts
seams.md                the upstream capture-seam proposal (feature floor note)
```

The TS mirror of the shared tile math is guarded by a real test
(`tests/equiv_test.ts`): the mod's planner and the stitcher's header are
compared number-for-number on a fixed input set, so the two sides cannot
drift. Build and run it:

```bash
cmake -S adapters/longshot/tests -B build/ls-tests && cmake --build build/ls-tests
node --experimental-strip-types adapters/longshot/tests/equiv_test.ts build/ls-tests/gen_tiles_cpp
```

## Development loop (needs a Longshot install on your PC)

The builder is the tool (docs/modding/agents.md in Longshot):

```bash
Longshot.exe mod check  adapters/longshot/mod --types   # manifest, lint, types
Longshot.exe mod test   adapters/longshot/mod           # fake game, seconds
Longshot.exe mod run    adapters/longshot/mod           # dev mode, hot reload
```

The tests drive the mod headlessly (claims taken, freeze + HUD held, pad
consumed, job cycling, plan-only capture without the seam) - see
`mod/test/photo.test.ts`.

## Known limits

- Longshot's overlays (CRT filter, colour-vision, HDR tone-map curve, FPS
  counter) are renderer-side; while the capture seam's `holdScreenEffects()`
  is the designed hold-off, until it lands capture is plan-only anyway, and
  the Insert menu's switches for those effects are manual.
- `hideGameHud()` hides the game's whole 2D layer (boxes and menus too),
  counted with other mods' - your toasts stay.
- A view far from Link shows what the game has loaded there, which may be
  nothing; the eye is clamped to 90 km so `setPosition` never refuses.
- 360 jobs use the pole faces at ±87.5° with a widened 95° FOV (both engines'
  `lookAt` degenerates at ±90° - the Dusklight lesson kept verbatim).
