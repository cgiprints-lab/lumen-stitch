// Job planning: turn a camera pose + a capture viewport into the Lumen job
// manifest (docs/adapter-contract.md, `lumenJob: 1`) the shared stitcher
// consumes, and the flat tile list the capture runner drives.
//
// Pure module (no "longshot/*" imports): it is the unit under the tile-math
// equivalence test and the mod's own plan tests.

import { cube360Faces, deg, kCube360Fovy, kCube360PoleFovy, makeTile, rad } from "./tilemath.ts";
import type { Master, Tile } from "./tilemath.ts";

export type Layout = "gigapixel" | "cube360" | "stereo";

export interface CameraPose {
  yaw: number;    // radians
  pitch: number;  // radians
  fovy: number;   // degrees, vertical
}

export interface TileOut {
  file: string;
  row: number;
  col: number;
  yaw: number;    // radians
  pitch: number;  // radians
  fovy: number;   // degrees
  width: number;
  height: number;
  eye?: number[]; // relative to the master eye (stereo only)
  rect: number[]; // [x0, y0, x1, y1] inside the master image
}

export interface JobOut {
  lumenJob: 1;
  layout: Layout;
  scale: number;
  colorFormat: number;
  master: { yaw: number; pitch: number; fovy: number; vw: number; vh: number; width: number; height: number };
  tiles: TileOut[];
}

// Reference cap from the contract: keep strip-composited stitching in RAM.
export const kMaxMasterSide = 65536;

// The capture pose drives the tile grid: each tile is itself a symmetric
// pinhole (Longshot's camera.override setAngles + setFov expresses exactly
// that), aimed at its master sub-rect. Shared-eye rule: gigapixel tiles
// never translate.
export function buildGigapixel(
  pose: CameraPose, vw: number, vh: number, scale: number, id: string, marginNative: number = 4,
): { job: JobOut; poses: Tile[] } {
  if (scale < 1) scale = 1;
  const width = vw * scale, height = vh * scale;
  const m: Master = { yaw: pose.yaw, pitch: pose.pitch, fovy: pose.fovy, vw, vh, width, height };
  const job: JobOut = {
    lumenJob: 1, layout: "gigapixel", scale, colorFormat: 2,
    master: { yaw: pose.yaw, pitch: pose.pitch, fovy: pose.fovy, vw, vh, width, height },
    tiles: [],
  };
  const poses: Tile[] = [];
  for (let row = 0; row < scale; ++row) {
    for (let col = 0; col < scale; ++col) {
      const x0 = col * vw, x1 = (col + 1) * vw;
      const y0 = row * vh, y1 = (row + 1) * vh;
      const t = makeTile(m, x0, y0, x1, y1, row, col, marginNative);
      poses.push(t);
      job.tiles.push({
        file: `lumen-${id}-r${row}-c${col}.png`,
        row, col, yaw: t.yaw, pitch: t.pitch, fovy: t.fovy, width: t.w, height: t.h,
        rect: [x0, y0, x1, y1],
      });
    }
  }
  return { job, poses };
}

// Six faces around the capture eye; atlas slots (4 x 2) are manifest
// bookkeeping, and the pole faces stop at 87.5 degrees with a widened 95
// degree vertical FOV (the lookAt degeneracy at +/-90 in both engines).
export function buildCube360(pose: CameraPose, vw: number, vh: number, id: string): { job: JobOut; poses: (FaceOrTilePose)[] } {
  const faces = cube360Faces(pose.yaw);
  const job: JobOut = {
    lumenJob: 1, layout: "cube360", scale: 1, colorFormat: 2,
    master: { yaw: pose.yaw, pitch: 0.0, fovy: kCube360Fovy, vw, vh, width: 4 * vw, height: 2 * vh },
    tiles: [],
  };
  const poses: (FaceOrTilePose)[] = [];
  faces.forEach((f, n) => {
    const pole = f.pitch !== 0.0;
    const fovy = pole ? kCube360PoleFovy : kCube360Fovy;
    poses.push({ yaw: f.yaw, pitch: f.pitch, fovy });
    job.tiles.push({
      file: `lumen-${id}-f${n}.png`,
      row: f.row, col: f.col, yaw: f.yaw, pitch: f.pitch, fovy, width: vw, height: vh,
      rect: [f.col * vw, f.row * vh, (f.col + 1) * vw, (f.row + 1) * vh],
    });
  });
  return { job, poses };
}

export interface FaceOrTilePose { yaw: number; pitch: number; fovy: number; }

// Left / center / right at the master FOV, eyes +-ipd/2 metres along the
// master camera's right axis; the stitcher pastes 1:1, so the rects do not
// overlap and the master extent is the side-by-side width.
export function buildStereo(
  pose: CameraPose, vw: number, vh: number, ipdMeters: number, id: string,
): { job: JobOut; poses: FaceOrTilePose[] } {
  const right = [Math.cos(pose.yaw), 0.0, -Math.sin(pose.yaw)];
  const half = ipdMeters / 2.0;
  const job: JobOut = {
    lumenJob: 1, layout: "stereo", scale: 1, colorFormat: 2,
    master: { yaw: pose.yaw, pitch: pose.pitch, fovy: pose.fovy, vw, vh, width: 3 * vw, height: vh },
    tiles: [],
  };
  const offsets = [-half, 0.0, half];
  const poses: FaceOrTilePose[] = [];
  offsets.forEach((s, n) => {
    poses.push({ yaw: pose.yaw, pitch: pose.pitch, fovy: pose.fovy });
    job.tiles.push({
      file: `lumen-${id}-e${n}.png`,
      row: 0, col: n, yaw: pose.yaw, pitch: pose.pitch, fovy: pose.fovy, width: vw, height: vh,
      eye: [right[0] * s, right[1] * s, right[2] * s],
      rect: [n * vw, 0, (n + 1) * vw, vh],
    });
  });
  return { job, poses };
}

// Re-exported for the runner's angle conversions (the game API speaks degrees).
export { deg, rad };
