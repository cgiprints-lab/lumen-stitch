# Adapter template

Copy this directory to `adapters/<your-game>/` and fill in the pieces. The
authoritative rules are in [`docs/adapter-contract.md`](../../docs/adapter-contract.md);
`adapters/dusklight/` is the finished reference for every step.

What an adapter actually is:

1. **`adapter.json`** — detection + install + artifact metadata. Replace
   `data_dirs` (where the game keeps user data per OS), `markers` (a file
   proving an install exists), `mod_subdir`, the integration id/bundle
   names, and the artifact slugs your CI will build.
2. **An in-game integration** — native mod / plugin for the game's own mod
   system. It implements, in contract terms:
   - session controller (hotkey, time freeze, auto-end on scene change),
   - free camera (an overridable camera per frame),
   - HUD-free capture at a pre-HUD point + GPU readback to RGBA8 PNG,
   - job drivers: gigapixel grid / cube360 faces / stereo trio, sharing
     `common/tilemath.h` for every pose it aims,
   - effect stripping during jobs (screen-space + temporal), snapshot /
     re-pin / restore,
   - the **job format writer**: tile PNGs + `<prefix>-<id>.json` written
     only when the whole job succeeded.
   `mod/integration.cc.example` is the structural outline; `mod/
   CMakeLists.example` shows the build shape (vendor `common/`, fetch or
   point at the game's SDK, produce the bundle).
3. **CI** — add a job that builds one bundle per declared platform slug,
   plus the game-version compile matrix (fallback axis). See
   `.github/workflows/build.yml` (the `mod:` job) for the pattern.

No stitcher or manager code changes are needed: `lumen stitch <job.json>`
and `lumen install --adapter <your-game>` work as soon as the manifest and
the job format conform. Validate against the contract's acceptance
checklist (FOV=90 seam test, cube/stereo references, watchdog behavior) —
`stitcher/test/seam_test.cpp` is directly reusable: feed your tile writer's
output to `lumen::stitch::run` and compare against its reference renders.

The `_template` id is deliberately not installable: it lists in
`lumen status` as a placeholder and detects nothing.
