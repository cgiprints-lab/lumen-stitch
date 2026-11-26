// Lumen stitcher acceptance tests (M2 contract).
//
//  1. tilemath round-trip: master <-> tile pinhole correspondence is exact
//     (same-eye projective geometry; Ansel's alignment contract).
//  2. seam fidelity: tiles rendered from one continuous synthetic scene,
//     stitched through the real pipeline (manifest -> composite -> BigTIFF
//     -> tile readback), compared against a direct reference render, with
//     extra scrutiny on pixels flanking tile seams. Uses a FOV=90 master.
//  3. BigTIFF structure: magic/version/IFD chain/tile tables/SubIFD mip
//     chain, and a decodable JPEG preview.
//  4. downsample path: composite_scaled box-average matches the exact master
//     grid downsample; run() level-0 dims are ceil(master / N).
//
// Exits non-zero on the first failure.
#include "stitch.h"

#include "bigtiff.h"

// stb implementations come from the stitch.cpp translation unit.
#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

// ---- synthetic scene: a smooth function of ray direction (the same
// function feeds both the tile renderers and the reference render, so any
// stitched/reference difference is pipeline error, not scene disagreement).
void scene(const double d[3], uint8_t* rgb) {
    const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double x = d[0] / n, y = d[1] / n, z = d[2] / n;
    rgb[0] = static_cast<uint8_t>(127.5 + 122.0 * std::sin(1.2 + 1.5 * x));
    rgb[1] = static_cast<uint8_t>(127.5 + 122.0 * std::sin(2.1 + 1.3 * y + 0.7 * z));
    rgb[2] = static_cast<uint8_t>(127.5 + 122.0 * std::cos(1.7 + 1.1 * z - 0.9 * x));
}

void master_ref(const lumen::tilemath::Master& m, double x, double y, uint8_t* rgb) {
    double d[3];
    lumen::tilemath::master_ray(m, x, y, d);
    scene(d, rgb);
}

// Render one tile image by projecting each tile pixel to a ray (the tile's
// own pinhole), then evaluating the scene - exactly what the game does for
// a captured sub-frustum.
std::vector<uint8_t> render_tile_rgba(const lumen::tilemath::Tile& t) {
    std::vector<uint8_t> img(static_cast<size_t>(t.w) * t.h * 4);
    double ft[3], rt[3], ut[3];
    lumen::tilemath::basis(t.yaw, t.pitch, ft, rt, ut);
    const double thty = std::tan(lumen::tilemath::rad(t.fovy) / 2.0);
    const double thtx = thty * static_cast<double>(t.w) / static_cast<double>(t.h);
    for (uint32_t j = 0; j < t.h; ++j) {
        const double my = (1.0 - (j + 0.5) / t.h * 2.0) * thty;
        for (uint32_t i = 0; i < t.w; ++i) {
            const double mx = ((i + 0.5) / t.w * 2.0 - 1.0) * thtx;
            double d[3];
            for (int k = 0; k < 3; ++k)
                d[k] = ft[k] + mx * rt[k] + my * ut[k];
            uint8_t* p = img.data() + (static_cast<size_t>(j) * t.w + i) * 4;
            scene(d, p);
            p[3] = 255;
        }
    }
    return img;
}

// ---- minimal BigTIFF structural reader (little-endian only, our writer)
struct IfdEntry {
    uint16_t tag, type;
    uint64_t count, value;
};

uint64_t rd(std::FILE* f, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= static_cast<uint64_t>(std::fgetc(f)) << (8 * i);
    return v;
}

bool read_ifd(std::FILE* f, uint64_t off, std::vector<IfdEntry>& out, uint64_t& next) {
    if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0)
        return false;
    const uint64_t n = rd(f, 2);
    out.clear();
    for (uint64_t i = 0; i < n; ++i) {
        IfdEntry e;
        e.tag = static_cast<uint16_t>(rd(f, 2));
        e.type = static_cast<uint16_t>(rd(f, 2));
        e.count = rd(f, 8);
        e.value = rd(f, 8);
        out.push_back(e);
    }
    next = rd(f, 8);
    return true;
}

const IfdEntry* find_entry(const std::vector<IfdEntry>& ifd, uint16_t tag) {
    for (const auto& e : ifd)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

// ---------------------------------------------------------------- test 1

void test_round_trip() {
    std::printf("tilemath coverage + round-trip (FOV=90 master, 2x2 grid):\n");
    lumen::tilemath::Master m;
    m.yaw = 0.7;
    m.pitch = 0.15;
    m.fovy = 90;
    m.vw = 64;
    m.vh = 48;
    m.width = 128;
    m.height = 96;
    double worst_err = 0, worst_margin = 1e9;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            const auto t = lumen::tilemath::make_tile(m, c * 64, r * 48, (c + 1) * 64,
                (r + 1) * 48, r, c);
            const lumen::tilemath::RayMap map(m, t);
            // Coverage invariant: every master pixel owned by this tile maps
            // strictly inside the tile image with a full bilinear stencil
            // (what makes seam pixels interpolable from the right tile).
            for (double y = t.y0; y < t.y1 + 1; ++y) {
                for (double x = t.x0; x < t.x1 + 1; ++x) {
                    double px, py;
                    if (!map.eval(x, y, px, py)) {
                        check(false, "coverage: owner tile rejected its own master pixel");
                        return;
                    }
                    const double mx = std::min(px, static_cast<double>(t.w) - px);
                    const double my = std::min(py, static_cast<double>(t.h) - py);
                    worst_margin = std::min(worst_margin, std::min(mx, my));
                }
            }
            // Independent round-trip for tile pixels that lie within the
            // master footprint (the symmetric FOV cone may exceed it):
            // tile pixel -> tile ray -> master coordinates -> RayMap -> close.
            double ft[3], rt[3], ut[3];
            lumen::tilemath::basis(t.yaw, t.pitch, ft, rt, ut);
            const double thty = std::tan(lumen::tilemath::rad(t.fovy) / 2.0);
            const double thtx = thty * static_cast<double>(t.w) / static_cast<double>(t.h);
            double f[3], rr[3], u[3];
            lumen::tilemath::basis(m.yaw, m.pitch, f, rr, u);
            const double hy = std::tan(lumen::tilemath::rad(m.fovy) / 2.0);
            const double hx = hy * static_cast<double>(m.vw) / static_cast<double>(m.vh);
            for (double j = 0.5; j < t.h; j += 11.3) {
                for (double i = 0.5; i < t.w; i += 17.7) {
                    const double mx = (i / t.w * 2.0 - 1.0) * thtx;
                    const double my = (1.0 - j / t.h * 2.0) * thty;
                    double d[3];
                    for (int k = 0; k < 3; ++k)
                        d[k] = ft[k] + mx * rt[k] + my * ut[k];
                    const double den = lumen::tilemath::dot(d, f);
                    if (!(den > 1e-6))
                        continue;  // ray outside the master frustum: not sampled
                    const double mxm = lumen::tilemath::dot(d, rr) / den;
                    const double mym = lumen::tilemath::dot(d, u) / den;
                    if (std::fabs(mxm) > hx || std::fabs(mym) > hy)
                        continue;  // beyond the master image: never sampled
                    const double x = (mxm / hx + 1.0) * 0.5 * m.width - 0.5;
                    const double y = (1.0 - mym / hy) * 0.5 * m.height - 0.5;
                    double px, py;
                    if (!map.eval(x, y, px, py)) {
                        check(false, "raymap eval failed on in-footprint ray");
                        return;
                    }
                    worst_err =
                        std::max(worst_err, std::max(std::fabs(px - i), std::fabs(py - j)));
                }
            }
        }
    }
    check(worst_margin >= 1.0, "owner tiles cover their own rects with bilinear stencil "
            "margin >= 1 px (worst " + std::to_string(worst_margin) + ")");
    check(worst_err < 1e-9, "in-footprint tile<->master correspondence closes to < 1e-9 px "
            "(worst " + std::to_string(worst_err) + ")");
}

// ---------------------------------------------------------------- test 2 + 3

void test_stitch(const std::filesystem::path& tmp) {
    std::printf("stitch seam fidelity + BigTIFF structure (FOV=90, 2x2 tiles):\n");
    lumen::tilemath::Master m;
    m.yaw = 0.35;
    m.pitch = -0.1;
    m.fovy = 90;
    m.vw = 64;
    m.vh = 48;
    const uint32_t S = 2;
    m.width = m.vw * S;
    m.height = m.vh * S;

    // Manifest + tile PNGs.
    std::string manifest = "{\"lumenJob\":1,\"scale\":2,\"master\":{\"yaw\":0.35,\"pitch\":-0.1,"
                           "\"fovy\":90,\"vw\":64,\"vh\":48,\"width\":128,\"height\":96},"
                           "\"tiles\":[";
    for (uint32_t r = 0; r < S; ++r) {
        for (uint32_t c = 0; c < S; ++c) {
            const auto t = lumen::tilemath::make_tile(m, c * 64, r * 48, (c + 1) * 64,
                (r + 1) * 48, static_cast<int>(r), static_cast<int>(c));
            const auto img = render_tile_rgba(t);
            const std::string fname = "tile-" + std::to_string(r) + "-" + std::to_string(c) + ".png";
            if (!stbi_write_png((tmp / fname).string().c_str(), static_cast<int>(t.w),
                    static_cast<int>(t.h), 4, img.data(), static_cast<int>(t.w) * 4)) {
                check(false, "tile png write failed");
                return;
            }
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "%s{\"file\":\"%s\",\"row\":%d,\"col\":%d,\"yaw\":%.17g,\"pitch\":%.17g,"
                "\"fovy\":%.17g,\"width\":%u,\"height\":%u,\"rect\":[%u,%u,%u,%u]}",
                (r || c) ? "," : "", fname.c_str(), t.row, t.col, t.yaw, t.pitch, t.fovy, t.w,
                t.h, t.x0, t.y0, t.x1, t.y1);
            manifest += buf;
        }
    }
    manifest += "]}";
    const auto manifest_path = tmp / "job.json";
    {
        std::ofstream out(manifest_path, std::ios::binary);
        out << manifest;
    }

    lumen::stitch::Options opt;
    opt.out = tmp / "out.tiff";
    opt.preview_max = 64;
    opt.min_mip_edge = 16;
    std::string err;
    if (!lumen::stitch::run(manifest_path, opt, err)) {
        check(false, "stitch::run: " + err);
        return;
    }
    check(true, "stitch::run completed");

    // Read the stitched image back through the tile tables (also exercises
    // the writer's offset bookkeeping).
    std::FILE* f = std::fopen(opt.out.string().c_str(), "rb");
    if (!f) {
        check(false, "cannot reopen output tiff");
        return;
    }
    const bool magic = rd(f, 2) == 0x4949 && rd(f, 2) == 43 && rd(f, 2) == 8;
    check(magic, "BigTIFF header (II, 43, 8)");
    rd(f, 2); // reserved offset-size padding half of the 8-byte offset field
    const uint64_t ifd0_off = rd(f, 8);
    std::vector<IfdEntry> ifd0;
    uint64_t next = 0;
    const bool parsed = read_ifd(f, ifd0_off, ifd0, next);
    check(parsed, "IFD0 parses");
    auto get = [&](uint16_t tag) -> uint64_t {
        const auto* e = find_entry(ifd0, tag);
        return e ? e->value : 0;
    };
    check(get(256) == 128 && get(257) == 96, "level 0 dimensions 128x96");
    check(get(322) == 256 && get(323) == 256, "tile dimensions 256");
    const auto* offs = find_entry(ifd0, 324);
    const auto* cnts = find_entry(ifd0, 325);
    check(offs && offs->count == 1 && cnts && cnts->count == 1, "1x1 tile grid tables");
    std::vector<uint8_t> raster(static_cast<size_t>(128) * 96 * 3);
    if (offs && cnts) {
        std::fseek(f, static_cast<long>(offs->value), SEEK_SET);
        const uint64_t tile_off = rd(f, 8);
        std::fseek(f, static_cast<long>(cnts->value), SEEK_SET);
        const uint64_t tile_cnt = rd(f, 8);
        check(tile_cnt == 128ull * 96 * 3, "tile byte count = full image (uncompressed RGB)");
        std::fseek(f, static_cast<long>(tile_off), SEEK_SET);
        std::vector<uint8_t> data(tile_cnt);
        if (std::fread(data.data(), 1, tile_cnt, f) != tile_cnt)
            check(false, "tile data short read");
        // 128x96 < 256x256: one tile holding rows 0..95 with row stride 128*3.
        for (uint64_t y = 0; y < 96; ++y)
            for (uint64_t x = 0; x < 128; ++x)
                for (int c = 0; c < 3; ++c)
                    raster[(y * 128 + x) * 3 + c] = data[(y * 128 + x) * 3 + c];
    }
    const auto* subifds = find_entry(ifd0, 33005);
    check(subifds && subifds->count >= 2, "SubIFD mip chain present (>= 2 levels)");
    if (subifds) {
        std::fseek(f, static_cast<long>(subifds->value), SEEK_SET);
        const uint64_t mip0 = rd(f, 8);
        std::vector<IfdEntry> mip_ifd;
        uint64_t mip_next = 0;
        if (read_ifd(f, mip0, mip_ifd, mip_next)) {
            const auto* w = find_entry(mip_ifd, 256);
            const auto* h = find_entry(mip_ifd, 257);
            check(w && w->value == 64 && h && h->value == 48, "mip level 1 is 64x48");
            check(find_entry(mip_ifd, 33005) == nullptr, "reduced IFD has no SubIFDs tag");
        } else {
            check(false, "mip IFD parses");
        }
    }
    std::fclose(f);

    // Seam fidelity: reference render vs stitched raster over every master
    // pixel; boundary flanks matter most.
    int worst = 0;
    int worst_seam = 0;
    for (uint32_t y = 0; y < 96; ++y) {
        for (uint32_t x = 0; x < 128; ++x) {
            uint8_t ref[3];
            master_ref(m, x, y, ref);
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(raster[(y * 128 + x) * 3 + c]) -
                    static_cast<int>(ref[c]));
                worst = std::max(worst, d);
                const bool seam = (x == 63 || x == 64 || y == 47 || y == 48);
                if (seam)
                    worst_seam = std::max(worst_seam, d);
            }
        }
    }
    check(worst <= 3, "stitched matches reference everywhere within 3/255 (worst " +
            std::to_string(worst) + ")");
    check(worst_seam <= 3, "seam flanks match within 3/255 (worst " +
            std::to_string(worst_seam) + ")");

    const auto preview = tmp / "out.jpg";
    int pw = 0, ph = 0, pc = 0;
    unsigned char* pimg = stbi_load(preview.string().c_str(), &pw, &ph, &pc, 3);
    check(pimg != nullptr, "JPEG preview decodes");
    if (pimg)
        stbi_image_free(pimg);
}

// ---------------------------------------------------------------- test 4

// Downsample path: composite_scaled(N) must equal the exact box downsample of
// the master grid (not a resample of the stitched raster), and run() must
// honour ceil(master / N) level-0 dimensions.
void test_downscale(const std::filesystem::path& tmp) {
    std::printf("downsample path (master/2 box downsample vs exact reference):\n");
    lumen::tilemath::Master m;
    m.yaw = 0.35;
    m.pitch = -0.1;
    m.fovy = 90;
    m.vw = 64;
    m.vh = 48;
    m.width = 128;
    m.height = 96;

    lumen::stitch::Job job;
    std::string err;
    if (!lumen::stitch::load_job(tmp / "job.json", job, err)) {
        check(false, "load_job: " + err);
        return;
    }

    const uint32_t f = 2, OW = 64, OH = 48;
    std::vector<uint8_t> raster(static_cast<size_t>(OW) * OH * 3);
    uint64_t rows = 0;
    bool size_ok = true;
    const bool ok = lumen::stitch::composite_scaled(job, f,
        [&](uint64_t y0, uint64_t n, const uint8_t* rgb) {
            for (uint64_t r = 0; r < n; ++r) {
                if (y0 + r >= OH)
                    size_ok = false;
                else
                    std::copy(rgb + r * OW * 3, rgb + (r + 1) * OW * 3,
                        raster.data() + (y0 + r) * OW * 3);
                ++rows;
            }
            return true;
        },
        {}, err);
    check(ok && size_ok && rows == OH, "composite_scaled(2): 48 rows, 64 px stride, no error");
    if (!ok)
        return;

    // Exact expectation: box average of the scene at the master pixel centres
    // the output pixel covers. Bilinear tile sampling carries the same
    // <= 3/255 slop already quantified by the 1:1 seam test.
    int worst = 0;
    for (uint32_t y = 0; y < OH; ++y) {
        for (uint32_t x = 0; x < OW; ++x) {
            uint32_t acc[3] = {0, 0, 0};
            for (uint32_t j = 0; j < f; ++j) {
                for (uint32_t i = 0; i < f; ++i) {
                    uint8_t ref[3];
                    master_ref(m, x * f + i, y * f + j, ref);
                    for (int c = 0; c < 3; ++c)
                        acc[c] += ref[c];
                }
            }
            for (int c = 0; c < 3; ++c) {
                const int want = static_cast<int>((acc[c] + 2) / 4);
                const int got = raster[(static_cast<size_t>(y) * OW + x) * 3 + c];
                worst = std::max(worst, std::abs(got - want));
            }
        }
    }
    check(worst <= 3, "box downsample matches exact reference within 3/255 (worst " +
            std::to_string(worst) + ")");

    // run() with --downscale: level-0 dims are ceil(master / N).
    lumen::stitch::Options opt;
    opt.out = tmp / "ds.tiff";
    opt.downscale = 3;  // ceil(128/3)=43, ceil(96/3)=32
    opt.preview_max = 64;
    opt.min_mip_edge = 16;
    if (!lumen::stitch::run(tmp / "job.json", opt, err)) {
        check(false, "stitch::run(downscale=3): " + err);
        return;
    }
    std::FILE* fd = std::fopen(opt.out.string().c_str(), "rb");
    if (!fd) {
        check(false, "cannot reopen downscaled output");
        return;
    }
    rd(fd, 2);
    rd(fd, 2);
    rd(fd, 2);
    rd(fd, 2);  // magic + version + offset-size half
    std::vector<IfdEntry> ifd0;
    uint64_t next = 0;
    if (read_ifd(fd, rd(fd, 8), ifd0, next)) {
        const auto* w = find_entry(ifd0, 256);
        const auto* h = find_entry(ifd0, 257);
        check(w && w->value == 43 && h && h->value == 32, "run(downscale=3) level 0 is 43x32");
    } else
        check(false, "downscaled IFD0 parses");
    std::fclose(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path tmp = argc > 1 ? argv[1] : std::filesystem::temp_directory_path();
    tmp = tmp / "lumen-seam-test";
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);

    test_round_trip();
    test_stitch(tmp);
    test_downscale(tmp);

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
