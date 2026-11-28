// Tile-math equivalence test: the mod's TypeScript planner (mod/src/plan.ts
// + tilemath.ts) must agree with the stitcher's C++ header (common/tilemath.h)
// on every geometry number the job manifest carries. Run as:
//
//   node --experimental-strip-types adapters/longshot/tests/equiv_test.ts <gen_tiles_cpp>
//   (or plain `node ... equiv_test.ts <gen>` on Node >= 23.6)
//
// Exits 0 when every number matches within 1e-12 (radians) / 1e-9 (degrees).

import { buildCube360, buildGigapixel, buildStereo } from "../mod/src/plan.ts";
import { deg } from "../mod/src/tilemath.ts";

const GEN = process.argv[2];
if (!GEN) {
    console.error("usage: equiv_test.ts <path to gen_tiles_cpp>");
    process.exit(2);
}

const { execFileSync } = await import("node:child_process");
const cpp = JSON.parse(execFileSync(GEN, { encoding: "utf8" }));

const MASTER = { yaw: 0.5, pitch: 0.2, fovy: 73.5, vw: 1280, vh: 720, width: 1280 * 4, height: 720 * 4 };
const pose = { yaw: MASTER.yaw, pitch: MASTER.pitch, fovy: MASTER.fovy };

let failures = 0;
function near(a: number, b: number, tol: number, what: string): void {
    if (!(Math.abs(a - b) <= tol)) {
        console.error(`MISMATCH ${what}: ts=${a} cpp=${b}`);
        ++failures;
    }
}

// --- gigapixel: every tile's pose, size and master rect ----------------------
const grid = buildGigapixel(pose, MASTER.vw, MASTER.vh, 4, "T").poses;
if (grid.length !== cpp.grid.length) {
    console.error(`grid count ts=${grid.length} cpp=${cpp.grid.length}`);
    ++failures;
}
cpp.grid.forEach((c: any, n: number) => {
    const t = grid[n];
    if (!t) return;
    near(t.row, c.row, 0, `grid[${n}].row`);
    near(t.col, c.col, 0, `grid[${n}].col`);
    near(t.yaw, c.yaw, 1e-12, `grid[${n}].yaw`);
    near(t.pitch, c.pitch, 1e-12, `grid[${n}].pitch`);
    near(t.fovy, c.fovy, 1e-9, `grid[${n}].fovy`);
    near(t.w, c.w, 0, `grid[${n}].w`);
    near(t.h, c.h, 0, `grid[${n}].h`);
    [t.x0, t.y0, t.x1, t.y1].forEach((v, i) => near(v, c.rect[i], 0, `grid[${n}].rect[${i}]`));
});

// --- cube360: faces, pole widening, atlas slots ------------------------------
const cube = buildCube360(pose, MASTER.vw, MASTER.vh, "T").poses;
cube.forEach((f: any, n: number) => {
    const c = cpp.cube[n];
    near(f.yaw, c.yaw, 1e-12, `cube[${n}].yaw`);
    near(f.pitch, c.pitch, 1e-12, `cube[${n}].pitch`);
    near(f.fovy, c.fovy, 1e-9, `cube[${n}].fovy`);
    const face = buildCube360(pose, MASTER.vw, MASTER.vh, "T").job.tiles[n];
    near(face.row, c.row, 0, `cube[${n}].row`);
    near(face.col, c.col, 0, `cube[${n}].col`);
});

// --- stereo: eye offsets along the master right axis -------------------------
const stereo = buildStereo({ yaw: 0.5, pitch: 0.2, fovy: 60 }, MASTER.vw, MASTER.vh, 0.06, "T").job.tiles;
stereo.forEach((t, n) => {
    const c = cpp.stereo[n];
    near(t.yaw, c.yaw, 1e-12, `stereo[${n}].yaw`);
    near(t.pitch, c.pitch, 1e-12, `stereo[${n}].pitch`);
    near(t.fovy, c.fovy, 1e-9, `stereo[${n}].fovy`);
    t.eye!.forEach((v, i) => near(v, c.eye[i], 1e-12, `stereo[${n}].eye[${i}]`));
});

// --- the contract's own invariants (belt on top of the comparison) -----------
{
    const { job } = buildGigapixel({ yaw: 0, pitch: 0, fovy: 90 }, 128, 128, 2, "S");
    const half = Math.tan((45 * Math.PI) / 180);
    // FOV-90 rule sanity: sub-rect + margin keeps tiles strictly narrower than master.
    for (const t of job.tiles)
        if (!(t.fovy > 40 && t.fovy < 90)) { console.error(`tile fovy out of band: ${t.fovy}`); ++failures; }
    void half;
    // rects partition the master exactly:
    for (let y = 0; y < 2; ++y)
        for (let x = 0; x < 2; ++x) {
            const t = job.tiles[y * 2 + x];
            const exp = [x * 128, y * 128, (x + 1) * 128, (y + 1) * 128];
            t.rect.forEach((v, i) => near(v, exp[i], 0, `partition[${y}][${x}][${i}]`));
        }
    void deg;
}

if (failures) {
    console.error(`equiv_test: ${failures} mismatch(es)`);
    process.exit(1);
}
console.log("equiv_test: tile math agrees (grid 16, cube 6, stereo 3)");
