// Lumen Photo Suite for Longshot: free camera, world freeze, HUD-free capture,
// and the gigapixel / 360 / stereo jobs of the Lumen stitcher.
//
// Controller-first scheme (all remappable through the mod's settings in the
// Mods tab):
//   L3+R3 (or F8)        photo mode on/off - takes the camera, the world
//                        freezes, the HUD hides (each held item follows its
//                        setting and the game's feature flags)
//   right stick / mouse  look          left stick  fly        ZL/ZR up/down
//   R (or F5)            choose the job: single -> stereo -> gigapixel -> 360
//   ZR (or F6), or A     take the shot: runs the chosen job
//   B                    cancel a running job
//
// Capture itself rides the proposed capture seam (../seams.md): the mod loads
// without it and photo mode works fully; the capture button then logs the
// job's plan (and keeps it in the mod's profile storage) instead of writing
// tiles, and says so. Every subsystem probes its own feature at use.
//
// Everything taken is released on the way out, by the mod or by the game if
// the mod stops (same discipline as docs/modding/examples/photo-mode).

import { has, NotAvailableError } from "longshot";
import type { CameraOverride, Handle, Subscription, Vec3 } from "longshot";
import camera from "longshot/camera";
import game, { after, mapLoading, update } from "longshot/game";
import { action, pad, pointer } from "longshot/input";
import log from "longshot/log";
import { profile } from "longshot/storage";
import settings from "longshot/settings";
import { Box, Text, hideGameHud, mount } from "longshot/ui";
import { buildCube360, buildGigapixel, buildStereo, deg, kMaxMasterSide } from "./plan.ts";
import type { FaceOrTilePose, JobOut } from "./plan.ts";

// --- capture seam (proposed; absent until upstream lands it, ../seams.md) ---

interface CaptureFrameResult { file: string; width: number; height: number; colorFormat?: number; }
interface CaptureApi {
  viewport(): { width: number; height: number };
  folder(): string;
  frame(options: { file: string }): Promise<CaptureFrameResult>;
  writeText(options: { file: string; text: string }): Promise<void> | void;
  holdScreenEffects(): Handle;
}
let captureApi: CaptureApi | null = null;
let captureProbed = false;

function probeCapture(): void {
  if (captureProbed)
    return;
  captureProbed = true;
  if (!has("capture.frame")) {
    log.info("lumen: no capture seam in this build - photo mode works, capture logs plans");
    return;
  }
  import("longshot/capture").then((m: { default?: CaptureApi } & Partial<CaptureApi>) => {
    captureApi = m.default ?? (m as CaptureApi);
    log.info("lumen: capture seam present");
  }).catch(() => log.info("lumen: capture seam announced but not importable; plans only"));
}

// --- jobs -------------------------------------------------------------------

const JOBS = ["single", "stereo", "gigapixel", "360"] as const;
type JobKind = typeof JOBS[number];
let jobKind = JOBS.indexOf((profile.get("jobKind") as typeof JOBS[number]) ?? "single");

interface Run {
  kind: JobKind;
  id: string;
  job: JobOut | null;          // manifest for the grid/cube/stereo kinds
  poses: (FaceOrTilePose & { file: string })[];
  i: number;
  settle: number;              // frames to hold the pose before the readback
  pending: boolean;            // a frame() is in flight
  watchdog: number;            // seconds the in-flight readback has taken
  hold: Handle | null;         // screen-effect hold for the job's length
}
let run: Run | null = null;
let jobSeq = 0;

function nextJob(): void {
  jobKind = (jobKind + 1) % JOBS.length;
  profile.set("jobKind", JOBS[jobKind]);
  log.info(`lumen: job ${JOBS[jobKind]}`);
  toast(`job: ${JOBS[jobKind]}`);
}

function prevJob(): void {
  jobKind = (jobKind + JOBS.length - 1) % JOBS.length;
  profile.set("jobKind", JOBS[jobKind]);
  log.info(`lumen: job ${JOBS[jobKind]}`);
  toast(`job: ${JOBS[jobKind]}`);
}

function cfgSettle(): number { return Math.max(1, Math.min(10, Number(settings.get("settleFrames")) || 4)); }
function cfgSpeed(): number { return Math.max(1, Math.min(120, Number(settings.get("speed")) || 8)); }
function cfgGridScale(): number { return Math.max(2, Math.min(33, Number(settings.get("gridScale")) || 4)); }
function cfgIpdM(): number { return Math.max(0.02, Math.min(0.14, (Number(settings.get("stereoIpdCm")) || 6) / 100)); }

function jobId(): string {
  return `${Math.trunc(after() * 1000)}-${++jobSeq}`;
}

// The poses the job would shoot from the view's current state.
function planJob(vw: number, vh: number, id: string): { kind: JobKind; job: JobOut | null; poses: FaceOrTilePose[] } {
  const pose = { yaw: mode!.yaw, pitch: mode!.pitch, fovy: mode!.fov };
  if (JOBS[jobKind] === "single")
    return { kind: "single", job: null, poses: [{ ...pose, file: `lumen-${id}-full.png` }] };
  if (JOBS[jobKind] === "stereo") {
    const { job, poses } = buildStereo(pose, vw, vh, cfgIpdM(), id);
    return { kind: "stereo", job, poses };
  }
  if (JOBS[jobKind] === "gigapixel") {
    const cap = Math.max(2, Math.floor(kMaxMasterSide / Math.max(vw, vh)));
    const { job, poses } = buildGigapixel(pose, vw, vh, Math.min(cfgGridScale(), cap), id);
    return { kind: "gigapixel", job, poses };
  }
  const { job, poses } = buildCube360(pose, vw, vh, id);
  return { kind: "360", job, poses };
}

function startCapture(): void {
  if (!mode) {
    log.info("lumen: turn on photo mode first (L3+R3 / F8) - capture shoots from your own camera");
    return;
  }
  if (run) {
    log.info("lumen: a job is already running (B cancels it)");
    return;
  }
  probeCapture();
  const id = jobId();
  if (!captureApi) {
    // Feature floor without the seam: log the exact plan; stitch it once the
    // seam lands. The plan also rides in profile storage for recovery.
    const { kind, job } = planJob(1280, 720, id);   // viewport unknown here: assumed, flagged
    if (job) {
      profile.set("lastPlan", JSON.stringify(job));
      log.info(`lumen: plan ${kind} ${job.tiles.length} tiles from fov ${mode.fov.toFixed(1)}deg `
             + `(viewport assumed 1280x720) - no capture seam in this build, nothing written`);
    } else
      log.info("lumen: plan single frame - no capture seam in this build, nothing written");
    toast("capture needs the capture seam (see README)");
    return;
  }
  const vp = captureApi.viewport();
  const { kind, job, poses } = planJob(vp.width, vp.height, id);
  const files = job ? job.tiles.map((t) => t.file) : [`lumen-${id}-full.png`];
  const withFiles = poses.map((p, n) => ({ ...p, file: files[n] }));
  let hold: Handle | null = null;
  try {
    hold = captureApi.holdScreenEffects();   // CRT / colour-vision / FPS counter / temporal passes off
  } catch (err) {
    if (!(err instanceof NotAvailableError))
      throw err;
  }
  run = { kind, id, job, poses: withFiles, i: 0, settle: cfgSettle(), pending: false, watchdog: 0, hold };
  applyPose(0);
  log.info(`lumen: job ${kind} started, ${withFiles.length} frames, settle ${cfgSettle()}`);
  toast(`capturing ${kind}: ${withFiles.length} frames`);
}

function applyPose(i: number): void {
  const p = run!.poses[i];
  mode!.view.setAngles(deg(p.yaw), deg(p.pitch));
  mode!.view.setFov(Math.max(5, Math.min(140, p.fovy)));
}

function cancelRun(why: string): void {
  if (!run)
    return;
  if (run.hold)
    run.hold[Symbol.dispose]();
  log.info(`lumen: job ${run.kind} cancelled (${why}) - nothing written`);
  run = null;
}

function finishRun(): void {
  const r = run!;
  if (r.job && captureApi) {
    const name = `lumen-${r.id}.json`;
    try {
      const out = captureApi.writeText({ file: name, text: JSON.stringify(r.job) });
      if (out && typeof out.then === "function")
        out.catch((e: Error) => log.info(`lumen: manifest write failed: ${e}`));
    } catch (err) {
      if (!(err instanceof NotAvailableError))
        throw err;
      log.info(`lumen: no writeText on the seam; manifest for ${name}: ${JSON.stringify(r.job)}`);
    }
  }
  if (r.hold)
    r.hold[Symbol.dispose]();
  log.info(`lumen: job ${r.kind} done -> ${captureApi ? captureApi.folder() : "?"}`);
  toast(`${r.kind} done`);
  run = null;
}

function jobFault(what: string): void {
  cancelRun(what);
  toast("capture failed; nothing written");
}

function stepRun(f: { dt: number }): void {
  const r = run!;
  if (r.pending) {
    r.watchdog += f.dt;
    if (r.watchdog > 12)
      jobFault("readback watchdog");     // a stalled readback must never hang the job
    return;
  }
  if (r.settle > 0) {
    r.settle -= 1;                       // present the pose before the snapshot
    return;
  }
  if (!captureApi) {
    jobFault("capture seam vanished mid-job");
    return;
  }
  const p = r.poses[r.i];
  r.pending = true;
  r.watchdog = 0;
  captureApi.frame({ file: p.file }).then((res: CaptureFrameResult) => {
    if (run !== r)
      return;                            // cancelled or replaced while the readback ran
    r.pending = false;
    if (r.job && r.job.tiles[r.i]) {     // record the viewport actually rendered
      r.job.tiles[r.i].width = res.width;
      r.job.tiles[r.i].height = res.height;
      if (typeof res.colorFormat === "number")
        r.job.colorFormat = res.colorFormat;
    }
    r.i += 1;
    if (r.i >= r.poses.length) {
      finishRun();
      return;
    }
    applyPose(r.i);
    r.settle = cfgSettle();
  }).catch((e: Error) => {
    if (run !== r)
      return;
    jobFault(`frame ${p.file}: ${e}`);
  });
}

// --- photo mode ---------------------------------------------------------------

interface PhotoMode {
  view: CameraOverride;
  held: (Handle | Subscription)[];
  eye: Vec3;
  yaw: number;      // radians
  pitch: number;    // radians
  fov: number;      // degrees
  padNow: { a: boolean; b: boolean; dpadRight: boolean; dpadLeft: boolean };
  padWas: { a: boolean; b: boolean; dpadRight: boolean; dpadLeft: boolean };
  stick: { lx: number; ly: number; rx: number; ry: number; zl: boolean; zr: boolean };
}
let mode: PhotoMode | null = null;

const TURN = 1.6;          // radians a second at full right stick
const MOUSE_TURN = 0.003;  // radians a mouse count
const EYE_CLAMP = 90000;   // metres: the game refuses an eye past 100 km

function enter(): void {
  probeCapture();
  const now = camera.state();
  if (!now)
    return log.info("lumen: no camera to take right now");
  const held: (Handle | Subscription)[] = [];
  const view = camera.override({ fov: now.fov ?? 50, look: "fixed", pitchMin: -89, pitchMax: 89 });
  held.push(view);
  if (settings.get("freeze") && has("game.pause"))
    held.push(game.pause());
  if (settings.get("hideHud")) {
    try {
      held.push(hideGameHud());
    } catch (err) {
      if (!(err instanceof NotAvailableError))
        throw err;
    }
  }
  const p = now.position, t = now.target;
  const dx = p.x - t.x, dy = p.y - t.y, dz = p.z - t.z;
  const state: PhotoMode = {
    view, held, eye: { ...p },
    yaw: Math.atan2(dx, dz), pitch: -Math.atan2(dy, Math.hypot(dx, dz)),
    fov: now.fov ?? 50,
    padNow: { a: false, b: false, dpadRight: false, dpadLeft: false },
    padWas: { a: false, b: false, dpadRight: false, dpadLeft: false },
    stick: { lx: 0, ly: 0, rx: 0, ry: 0, zl: false, zr: false },
  };
  held.push(pad.intercept((draft) => {
    state.stick.lx = draft.left.x; state.stick.ly = draft.left.y;
    state.stick.rx = draft.right.x; state.stick.ry = draft.right.y;
    state.stick.zl = draft.buttons.zl; state.stick.zr = draft.buttons.zr;
    state.padNow.a = draft.buttons.a; state.padNow.b = draft.buttons.b;
    state.padNow.dpadRight = draft.buttons.dpadRight; state.padNow.dpadLeft = draft.buttons.dpadLeft;
    draft.cancel();                       // Link sees nothing while the camera flies
  }, { exclusive: true }));
  try {
    const mouse = pointer.capture();
    held.push(mouse);
    mouse.moved.on((m) => {
      state.yaw -= m.dx * MOUSE_TURN;
      state.pitch = Math.max(-1.55, Math.min(1.55, state.pitch - m.dy * MOUSE_TURN));
    });
  } catch (err) {
    if (!(err instanceof NotAvailableError))
      throw err;
  }
  mode = state;
  log.info(`lumen: photo mode on - ${JOBS[jobKind]} job${captureApi ? "" : " (plans only: no capture seam)"}`);
}

function leave(): void {
  if (!mode)
    return;
  cancelRun("photo mode off");
  for (const h of mode.held.reverse())
    h[Symbol.dispose]();
  mode = null;
  log.info("lumen: photo mode off");
}

const actPhoto = action("photo");
const actCapture = action("capture");
const actJob = action("job");
actPhoto.pressed.on(() => (mode ? leave() : enter()));
actCapture.pressed.on(() => { if (!mode) startCapture(); });   // inside photo mode the intercept's A drives it
actJob.pressed.on(() => { if (!mode) nextJob(); });            // ... and R's chord only cycles from outside

// --- the frame loop -------------------------------------------------------------

let toastBroken = false;
function toast(msg: string): void {
  if (toastBroken)
    return;
  try {
    mount(Box({}, Text({}, msg)), { surface: "hud", region: "top", for: 3 });
  } catch {
    toastBroken = true;   // one shielded attempt; logs carry everything anyway
  }
}

update.on((f) => {
  try {
    if (!mode)
      return;
    const s = mode.stick;
    // job poses hold; free flight only between jobs
    if (!run) {
      mode.yaw -= s.rx * TURN * f.dt;
      mode.pitch = Math.max(-1.55, Math.min(1.55, mode.pitch + s.ry * TURN * f.dt));
      const fx = -Math.sin(mode.yaw), fz = -Math.cos(mode.yaw);
      const vy = (s.zr ? 1 : 0) - (s.zl ? 1 : 0);
      const sp = cfgSpeed();
      mode.eye.x = clampEye(mode.eye.x + (fx * s.ly - fz * s.lx) * sp * f.dt);
      mode.eye.z = clampEye(mode.eye.z + (fz * s.ly + fx * s.lx) * sp * f.dt);
      mode.eye.y = clampEye(mode.eye.y + vy * sp * f.dt);
    }
    // controller edges (the intercept's consumed buttons, sampled per frame)
    const n = mode.padNow, w = mode.padWas;
    if (n.dpadRight && !w.dpadRight) nextJob();
    if (n.dpadLeft && !w.dpadLeft) prevJob();
    if (n.a && !w.a) startCapture();
    if (n.b && !w.b && run) cancelRun("player");
    w.a = n.a; w.b = n.b; w.dpadRight = n.dpadRight; w.dpadLeft = n.dpadLeft;
    mode.view.setPosition(mode.eye);
    if (run) {
      // re-assert the job's pose each frame so a reload can never leak into a tile
      const p = run.poses[run.i];
      mode.view.setAngles(deg(p.yaw), deg(p.pitch));
      mode.view.setFov(Math.max(5, Math.min(140, p.fovy)));
      stepRun(f);
    } else {
      mode.view.setAngles(deg(mode.yaw), deg(mode.pitch));
      mode.view.setFov(mode.fov);
    }
  } catch (e) {
    // A fault here must not strand the game holding our claims: drop the mode.
    log.info(`lumen: photo loop faulted: ${e}`);
    leave();
  }
});

function clampEye(v: number): number { return Math.max(-EYE_CLAMP, Math.min(EYE_CLAMP, v)); }

// The session must not outlive the scene it was planned in (contract rule).
mapLoading.on(() => {
  if (run)
    cancelRun("map change");
  if (mode)
    leave();
});

log.info(`lumen: ready - photo mode L3+R3 / F8, job ${JOBS[jobKind]} (R cycles, ZR or A shoots)`);
