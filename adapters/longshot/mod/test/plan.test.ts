// Pure planner tests (no game driving needed): the manifest every job builds
// must satisfy the stitcher's contract (docs/adapter-contract.md) - layouts,
// rects, the shared-eye rule, the FOV-90 band, and the pole-face widening.

import type { Tests } from "longshot/testing";
import { buildCube360, buildGigapixel, buildStereo } from "../src/plan.ts";
import { kCube360PolePitch, rad } from "../src/tilemath.ts";

const near = (a: number, b: number, tol: number) => Math.abs(a - b) <= tol;

export default {
  "gigapixel: exact partition, narrowing tiles, shared eye": (game, assert) => {
    const { job, poses } = buildGigapixel({ yaw: 0, pitch: 0, fovy: 90 }, 128, 128, 2, "S");
    assert.equal(job.lumenJob, 1);
    assert.equal(job.layout, "gigapixel");
    assert.equal(job.tiles.length, 4);
    assert.deepEqual([job.master.width, job.master.height], [256, 256]);
    for (const t of job.tiles) {
      assert.equal(t.eye, undefined);                         // shared-eye rule
      assert.ok(t.fovy > 40 && t.fovy < 90, `tile fovy band: ${t.fovy}`);
      assert.equal(t.file.indexOf("/"), -1);                  // flat names
    }
    // quadrants are symmetric about the master axis:
    const [q00, q01, q10, q11] = job.tiles;
    assert.ok(near(q00.yaw, -q01.yaw, 1e-12), "yaw mirror");
    assert.ok(near(q00.pitch, -q10.pitch, 1e-12), "pitch mirror");
    assert.deepEqual(q00.rect, [0, 0, 128, 128]);
    assert.deepEqual(q11.rect, [128, 128, 256, 256]);
    assert.equal(poses.length, 4);
  },

  "cube360: six faces, poles at 87.5 degrees widened to 95": (game, assert) => {
    const { job } = buildCube360({ yaw: 0.25, pitch: 0, fovy: 60 }, 160, 120, "C");
    assert.equal(job.layout, "cube360");
    assert.equal(job.tiles.length, 6);
    assert.deepEqual([job.master.width, job.master.height], [640, 240]);
    const poles = job.tiles.filter((t) => t.pitch !== 0);
    assert.equal(poles.length, 2);
    for (const p of poles) {
      assert.ok(near(Math.abs(p.pitch), rad(kCube360PolePitch), 1e-12), "pole pitch");
      assert.equal(p.fovy, 95);
    }
    for (const s of job.tiles.filter((t) => t.pitch === 0))
      assert.equal(s.fovy, 90);
    // atlas rects are disjoint slots:
    const seen = new Set(job.tiles.map((t) => t.rect.join(",")));
    assert.equal(seen.size, 6);
  },

  "stereo: eyes along the master right, rects side by side, no overlap": (game, assert) => {
    const { job } = buildStereo({ yaw: 0, pitch: 0, fovy: 60 }, 320, 180, 0.06, "E");
    assert.equal(job.layout, "stereo");
    assert.equal(job.tiles.length, 3);
    assert.deepEqual([job.master.width, job.master.height], [960, 180]);
    const [L, C, R] = job.tiles;
    assert.ok(near(L.eye[1], 0, 1e-15) && near(L.eye[2], 0, 1e-15), "offset along right axis only (yaw 0)");
    assert.ok(near(L.eye[0], -0.03, 1e-15) && near(R.eye[0], 0.03, 1e-15), "+-ipd/2");
    assert.deepEqual(C.eye, [0, 0, 0]);
    assert.deepEqual(L.rect, [0, 0, 320, 180]);
    assert.deepEqual(R.rect, [640, 0, 960, 180]);
  },
} satisfies Tests;
