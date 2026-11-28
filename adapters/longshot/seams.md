# The capture seam: what Lumen needs from Longshot upstream

The photo-mode half of this adapter ships entirely inside the Mod API as it
stands in #2434 (`api = "0.1"`): `camera.override` is the
free camera and the tile aimer (angles + FOV), `game.pause()` is the freeze,
`hideGameHud()` is the HUD. **The one missing piece is reading the scene
back to files.** `game/modapi/schema/gfx.toml` says it plainly: *"nothing
reads its output back"*, and a mod's own file API is read-only.

This page is the proposal (draft for a Longshot tracking issue): one new
mod-facing module, `longshot/capture`, host-side by construction. It fits
precedents that already exist in the codebase - the work is exposing them,
not inventing them.

> **Where it lands (checked 2026-11-28):** the API this rides on is the Mod
> API of PR **#2434** (the game side and mod API in Rust, WIP, rebased on the
> new Rust builder): its `api::services` layout (`game/crates/api/src/services/`)
> and its schema generator are exactly where a `capture` service would sit,
> beside `input` and `camera`. The capture button's other needs
> (`camera.override` rigs with `setAngles`, `game.pause`, `hideGameHud`,
> `pad.intercept`, `pointer.capture`) all exist there today, and
> `docs/modding/examples/photo-mode` is the pattern this adapter follows.

## Precedents already in Longshot

| Piece | Where it already lives |
|---|---|
| A frame read to PNG atomically, menu/FPS-counter-free | `RX_VK_SHOTS` / `RX_VK_SHOT_GO` / `RX_VK_SHOT_W/H` (docs/dev/developing.md); the control bridge's `shot` (docs/dev/reference/control-bridge.md: written under another name and renamed, size read back from the IHDR) |
| The correct readback sequence | "CPU reads of GPU-written mapped memory take three steps: a copy-to-host-read barrier (`capture_host_read_barrier`), a wait on the owning fence or timeline value, and `invalidate_mapped`" (docs/dev/reference/architecture.md) |
| Atomic file writes | `hos_atomic_write` (host/hos_atomic_file.h): ".hosnew written and flushed, then moved over" |
| A writable folder for a named purpose | sandbox.md's documented procedure: new dir in `sbx_game_subdirs` / `writable_file` in `hos_sandbox_start` |
| Scene-size / format knowledge | sites `output.sceneSize/sceneWidth/sceneHeight`, `render.sceneFormat` (RGBA16F vs R11G11B10F) |
| Per-frame hold-off of screen passes | the crt / colour-vision / hdr / fps-counter system mods are each a named pass "after the game's 3D and before Longshot's overlays"; each already has an off state |
| Projection offsets done right | `jitter.viewRows`: the upscalers' jitter "adds to the third lane of the first two rows in the registers only: the game's own views keep their projection" - the right shape, and Lumen doesn't even need it (tile poses ride `camera.override`, a symmetric pinhole at the tile's yaw/pitch/FOV, shared eye) |

## What the seam should be

```js
import capture from "longshot/capture";        // has("capture.frame") gates it

capture.viewport();                            // { width, height } the scene really renders at
capture.folder();                              // the capture folder the calls below write into
capture.frame({ file });                       // -> Promise<{ file, width, height, colorFormat }>
capture.writeText({ file, text });             // the job manifest, into the same folder
capture.holdScreenEffects();                   // -> Handle, counted like pauses/hides
```

- **`capture.frame({ file })`**: resolve the current frame *at the point
  before Longshot's own overlays composite* (the control-bridge shot already
  excludes the Insert menu and FPS counter - same anchor, per call), through
  the documented barrier → fence-wait → `invalidate_mapped` path, encode
  PNG, `hos_atomic_write` it into the capture folder. Resolve with the size
  actually rendered and the scene's `colorFormat` tag (so an HDR path has a
  hook later). **Reject, never hang**: a readback that does not complete in
  a bounded time (suggest 10 s) rejects `HostError` - the caller's watchdog,
  but the host must also not queue unbounded captures.
- **`capture.writeText({ file, text })`**: same folder, same atomic write,
  `file` flat-named (no path separators), bounded size (say 4 MB like the
  mod file reads). It exists so the job manifest lands **last**, over a
  complete tile set - the stitcher's contract refuses to treat a partial
  job as stitchable, and the manifest is what marks a job complete.
- **`capture.holdScreenEffects()`**: one counted hold, like the 32-deep
  pause/hide counts: while held, the crt / colour-vision / FPS-counter
  passes and the HDR scene-curve patch stay off for the *capture* path, and
  the temporal upscalers' jitter is suppressed for the captured frames.
  Snapshot and re-assert per frame (a player toggle mid-job must not leak
  into a tile); restore at release.
- **The capture folder**: a new sandbox-writable game-folder dir,
  `captures/<mod owner>/` - follow sandbox.md's own procedure for adding
  one (`sbx_game_subdirs`, `writable_file`). Names are mod-namespace so
  two photo mods cannot trample each other.
- **Permission**: `capture.frame` - "can take pictures of the game and save
  them in its capture folder" (no network, writes confined to that folder;
  sits at `low`-`medium`). The feature name `has()` reports is
  `capture.frame`. Rate limits: a size cap on the folder (say 2 GB), a cap
  on frames in flight (the bridge already refuses with "too many waiting").

## Why this and not the alternatives

- **`hooks.unsafe` / `memory.*` (cheats tier)**: capture through game
  addresses or the renderer's internals would pin the mod to build
  `e0f3ccfa`-specific layouts - exactly the update-coupling this whole
  adapter exists to avoid.
- **A renderer shim / fork of Longshot's GPU layer**: works, but dies on
  every upstream renderer change and is the opposite of "Longshot devs keep
  pushing updates independently".
- **Extending the control bridge / `RX_VK_SHOTS`**: already host-only and
  timer/menu-oriented, with no per-frame pose sync - and mods deliberately
  lost their command surface (`longshot mod shot` is gone) for sandbox
  reasons. The module above is the mod-facing shape of that same machinery.

## What Lumen does the moment it lands

Zero adapter rewrite: `mod/src/main.ts` already probes `has("capture.frame")`
and gates the same state machine on it - settle frames, per-tile
`camera.override` poses (shared eye, no parallax; the FOV=90 seam contract),
watchdogged awaits, manifest written last with real viewport sizes. The
shared stitcher (`lumen stitch <job.json>`) turns the folder into a
gigapixel BigTIFF with a mip chain, an equirectangular 360, or a stereo
side-by-side.

## Acceptance from Lumen's side

When the seam exists, the adapter's own CI floor applies (docs/adapter-contract.md
§4): FOV=90 2x2 seam test ≤ 3/255 including seam flanks, cube360 equirect
match, stereo 1:1 exact, and the tile-math equivalence already in
`tests/equiv_test.ts`. The photo-mode half is testable today under
`Longshot.exe mod test` (fake game).
