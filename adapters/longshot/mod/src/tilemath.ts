// Lumen tile-capture geometry, TypeScript mirror of ../../../../common/tilemath.h.
//
// The stitcher reads the same model from the C++ header; the equivalence test
// (../tests/equiv_test.ts) compares this file's numbers against the header's
// through a generated fixture, so the two sides cannot drift silently.
//
// World convention matches Longshot (and Dusklight): Y-up, yaw about +Y,
// pitch from the XZ plane. Continuous pixel coordinates: pixel i spans
// [i, i+1), its center is i+0.5.
//
// Pure module: no "longshot/*" imports, so it also runs under plain Node.

export const kPi = 3.14159265358979323846;
export function rad(deg: number): number { return deg * kPi / 180.0; }
export function deg(r: number): number { return r * 180.0 / kPi; }

// Orthonormal basis for (yaw, pitch): forward, right, up (each [x, y, z]).
export function basis(yaw: number, pitch: number): { f: number[]; r: number[]; u: number[] } {
  const cy = Math.cos(yaw), sy = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  const f = [cp * sy, sp, cp * cy];
  const r = [cy, 0.0, -sy];
  const u = [-sp * sy, cp, -sp * cy];
  return { f, r, u };
}

export function dot(a: number[], b: number[]): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Master camera parameters (yaw/pitch radians, fovy degrees, vertical).
export interface Master {
  yaw: number;
  pitch: number;
  fovy: number;
  vw: number;  // native viewport width
  vh: number;  // native viewport height
  width: number;   // final image width  (vw * scale)
  height: number;  // final image height (vh * scale)
}

// Ray (unnormalized) through continuous master pixel (x, y).
export function masterRay(m: Master, x: number, y: number): number[] {
  const { f, r, u } = basis(m.yaw, m.pitch);
  const hy = Math.tan(rad(m.fovy) / 2.0);
  const hx = hy * m.vw / m.vh;
  const mx = ((x + 0.5) / m.width * 2.0 - 1.0) * hx;
  const my = (1.0 - (y + 0.5) / m.height * 2.0) * hy;
  return [
    f[0] + mx * r[0] + my * u[0],
    f[1] + mx * r[1] + my * u[1],
    f[2] + mx * r[2] + my * u[2],
  ];
}

// One captured tile (same fields/semantics as tilemath.h's Tile).
export interface Tile {
  row: number;
  col: number;
  yaw: number;    // radians
  pitch: number;  // radians
  fovy: number;   // degrees
  w: number;
  h: number;
  x0: number; y0: number; x1: number; y1: number; // master-pixel sub-rect, [x0,x1) x [y0,y1)
}

// Build the tile that owns master sub-rect [x0,x1) x [y0,y1), rendered at
// native viewport size. Its axis hits the rect center; its FOV covers the
// rect expanded by `marginNative` tile pixels (clamped at the master bounds),
// bounded by the max corner extent. Mirrors make_tile() in tilemath.h
// line for line, including the pitch clamp and the degenerate-ray skip.
export function makeTile(
  m: Master, x0: number, y0: number, x1: number, y1: number,
  row: number, col: number, marginNative: number = 4,
): Tile {
  const t: Tile = { row, col, yaw: 0, pitch: 0, fovy: 90, w: m.vw, h: m.vh, x0, y0, x1, y1 };

  const c = masterRay(m, (x0 + x1) / 2.0, (y0 + y1) / 2.0);
  t.yaw = Math.atan2(c[0], c[2]);
  t.pitch = Math.atan2(c[1], Math.sqrt(c[0] * c[0] + c[2] * c[2]));
  // Keep the tile pose inside the camera operator's flight clamp.
  if (t.pitch > 1.55) t.pitch = 1.55;
  if (t.pitch < -1.55) t.pitch = -1.55;

  const { f: ft, r: rt, u: ut } = basis(t.yaw, t.pitch);
  const aspect = m.vw / m.vh;
  const per = m.width / m.vw;           // master pixels per tile pixel
  const mx = marginNative * per;
  const xs = [x0 >= mx ? x0 - mx : 0.0, x1 + mx, x0 >= mx ? x0 - mx : 0.0, x1 + mx];
  const ys = [y0 >= mx ? y0 - mx : 0.0, y0 >= mx ? y0 - mx : 0.0, y1 + mx, y1 + mx];
  let maxTu = 0.0, maxTr = 0.0;
  for (let i = 0; i < 4; ++i) {
    const xc = xs[i] > m.width ? m.width : xs[i];
    const yc = ys[i] > m.height ? m.height : ys[i];
    const d = masterRay(m, xc, yc);
    const den = dot(d, ft);
    if (den <= 1e-9)
      continue;                         // rect edge at/behind the axis
    const tr = dot(d, rt) / den;
    const tu = dot(d, ut) / den;
    if (Math.abs(tu) > maxTu) maxTu = Math.abs(tu);
    if (Math.abs(tr) > maxTr) maxTr = Math.abs(tr);
  }
  const thty = Math.max(maxTu, maxTr / aspect);
  t.fovy = 2.0 * deg(Math.atan(thty));
  return t;
}

// --- cube 360 capture model (tilemath.h: same constants, same slots) -------
export const kCube360Fovy = 90.0;       // degrees, vertical (side faces)
export const kCube360PolePitch = 87.5;  // degrees: shy of the lookAt singularity
export const kCube360PoleFovy = 95.0;   // degrees, vertical (poles widen to keep the seam ring)

export interface FacePose { yaw: number; pitch: number; row: number; col: number; }

export function cube360Faces(yaw: number): FacePose[] {
  return [
    { yaw, pitch: 0.0, row: 0, col: 1 },                    // front
    { yaw: yaw + rad(90.0), pitch: 0.0, row: 0, col: 0 },   // right
    { yaw: yaw + rad(180.0), pitch: 0.0, row: 0, col: 3 },  // back
    { yaw: yaw - rad(90.0), pitch: 0.0, row: 0, col: 2 },   // left
    { yaw, pitch: rad(kCube360PolePitch), row: 1, col: 1 }, // up
    { yaw, pitch: -rad(kCube360PolePitch), row: 1, col: 3 },// down
  ];
}
