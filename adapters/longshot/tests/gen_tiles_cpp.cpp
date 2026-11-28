// Equivalence fixture: prints the tile geometry that common/tilemath.h
// computes for a fixed input set, as JSON on stdout. The adapter's
// equiv_test.ts runs this and compares it against the mod's TypeScript
// mirror (src/tilemath.ts), so the in-game planner and the stitcher's
// header cannot drift apart unnoticed.
//
// Deliberately plain (no JSON library): one array per layout, %.17g.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "tilemath.h"

using namespace lumen::tilemath;

static void print_grid(const Master& m, int scale) {
    std::printf("\"grid\":[");
    bool first = true;
    for (int row = 0; row < scale; ++row) {
        for (int col = 0; col < scale; ++col) {
            const uint32_t x0 = uint32_t(col) * m.vw, x1 = uint32_t(col + 1) * m.vw;
            const uint32_t y0 = uint32_t(row) * m.vh, y1 = uint32_t(row + 1) * m.vh;
            const Tile t = make_tile(m, x0, y0, x1, y1, row, col, 4);
            std::printf("%s{\"row\":%d,\"col\":%d,\"yaw\":%.17g,\"pitch\":%.17g,\"fovy\":%.17g,"
                        "\"w\":%u,\"h\":%u,\"rect\":[%u,%u,%u,%u]}",
                        first ? "" : ",", t.row, t.col, t.yaw, t.pitch, t.fovy, t.w, t.h,
                        t.x0, t.y0, t.x1, t.y1);
            first = false;
        }
    }
    std::printf("]");
}

static void print_cube(double yaw) {
    FacePose faces[6];
    cube360_faces(yaw, faces);
    std::printf(",\"cube\":[");
    for (int n = 0; n < 6; ++n) {
        const double fovy = faces[n].pitch != 0.0 ? kCube360PoleFovy : kCube360Fovy;
        std::printf("%s{\"n\":%d,\"yaw\":%.17g,\"pitch\":%.17g,\"fovy\":%.17g,\"row\":%d,\"col\":%d}",
                    n ? "," : "", n, faces[n].yaw, faces[n].pitch, fovy, faces[n].row, faces[n].col);
    }
    std::printf("]");
}

static void print_stereo(double yaw, double pitch, double fovy, double ipd) {
    const double r[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
    std::printf(",\"stereo\":[");
    const double half = ipd / 2.0;
    const double s[3] = {-half, 0.0, half};
    for (int n = 0; n < 3; ++n) {
        std::printf("%s{\"yaw\":%.17g,\"pitch\":%.17g,\"fovy\":%.17g,\"eye\":[%.17g,%.17g,%.17g]}",
                    n ? "," : "", yaw, pitch, fovy, r[0] * s[n], r[1] * s[n], r[2] * s[n]);
    }
    std::printf("]");
}

int main() {
    // The same fixed inputs equiv_test.ts feeds the TypeScript side.
    Master m;
    m.yaw = 0.5;
    m.pitch = 0.2;
    m.fovy = 73.5;
    m.vw = 1280;
    m.vh = 720;
    m.width = 1280 * 4;
    m.height = 720 * 4;
    std::printf("{");
    print_grid(m, 4);
    print_cube(0.5);
    print_stereo(0.5, 0.2, 60.0, 0.06);
    std::printf("}\n");
    return 0;
}
