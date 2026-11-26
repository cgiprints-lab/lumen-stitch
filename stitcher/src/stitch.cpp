// Gigapixel stitcher implementation. See stitch.h for the contract.
#include "stitch.h"

#include "bigtiff.h"

#include "json.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>

namespace lumen::stitch {

namespace {

std::optional<double> json_num(const JsonValue* v) {
    if (!v || v->kind != JsonValue::Kind::Number)
        return std::nullopt;
    return v->number;
}

std::optional<uint32_t> json_u32(const JsonValue* v) {
    auto d = json_num(v);
    if (!d || *d < 0)
        return std::nullopt;
    return static_cast<uint32_t>(*d);
}

// Bilinear sample from an RGBA tile. Continuous tile pixel space (pixel i
// spans [i, i+1), center i+0.5); indices clamp (owned pixels never hit the
// clamp thanks to the capture margin; master borders edge-clamp).
void sample_tile(const uint8_t* img, uint32_t w, uint32_t h, double px, double py, uint8_t* out) {
    const double sx = px - 0.5, sy = py - 0.5;
    const int x0 = static_cast<int>(std::floor(sx));
    const int y0 = static_cast<int>(std::floor(sy));
    const double fx = sx - x0, fy = sy - y0;
    const int xa = std::clamp(x0, 0, static_cast<int>(w) - 1);
    const int xb = std::clamp(x0 + 1, 0, static_cast<int>(w) - 1);
    const int ya = std::clamp(y0, 0, static_cast<int>(h) - 1);
    const int yb = std::clamp(y0 + 1, 0, static_cast<int>(h) - 1);
    for (int c = 0; c < 3; ++c) {
        const double p00 = img[(static_cast<size_t>(ya) * w + xa) * 4 + c];
        const double p10 = img[(static_cast<size_t>(ya) * w + xb) * 4 + c];
        const double p01 = img[(static_cast<size_t>(yb) * w + xa) * 4 + c];
        const double p11 = img[(static_cast<size_t>(yb) * w + xb) * 4 + c];
        const double v = (p00 * (1 - fx) + p10 * fx) * (1 - fy) + (p01 * (1 - fx) + p11 * fx) * fy;
        out[c] = static_cast<uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
    }
}

// Accumulates full-width raster rows into TIFF tile rows.
struct LevelSink {
    BigTiffWriter::Level dim;
    BigTiffWriter* tiff = nullptr;
    size_t level_index = 0;
    std::vector<uint8_t> rows;
    uint64_t rows_in_buf = 0;
    uint64_t rows_fed = 0;
    std::vector<uint8_t>* preview = nullptr;

    void feed(const uint8_t* row) {
        const size_t stride = static_cast<size_t>(dim.width) * 3;
        rows.insert(rows.end(), row, row + stride);
        ++rows_fed;
        ++rows_in_buf;
        if (preview)
            preview->insert(preview->end(), row, row + stride);
        if (rows_in_buf == dim.tileH)
            flush(false);
    }

    void flush(bool final) {
        if (!rows_in_buf)
            return;
        if (!final && rows_in_buf < dim.tileH)
            return;
        const uint64_t tx_count = (dim.width + dim.tileW - 1) / dim.tileW;
        const uint32_t h_rows = static_cast<uint32_t>(rows_in_buf);
        const size_t stride = static_cast<size_t>(dim.width) * 3;
        std::vector<uint8_t> tile;
        for (uint64_t tx = 0; tx < tx_count; ++tx) {
            const uint64_t x0 = tx * dim.tileW;
            const uint32_t tw = static_cast<uint32_t>(std::min<uint64_t>(dim.tileW, dim.width - x0));
            tile.resize(static_cast<size_t>(tw) * h_rows * 3);
            for (uint32_t y = 0; y < h_rows; ++y)
                std::memcpy(tile.data() + static_cast<size_t>(y) * tw * 3,
                    rows.data() + static_cast<size_t>(y) * stride + x0 * 3,
                    static_cast<size_t>(tw) * 3);
            tiff->writeTile(level_index, tile.data(), static_cast<uint64_t>(tile.size()));
        }
        rows.clear();
        rows_in_buf = 0;
    }
};

// Streams raster rows into the whole level chain: level L+1 rows are 2x2 box
// averages of level L rows, computed one row at a time (ceil-halving: the
// unpaired final row at any level self-pairs in finish()).
struct MipFeeder {
    std::vector<LevelSink*> sinks;
    std::vector<std::vector<uint8_t>> prev_src;  // prev_src[L] = previous row of level L-1

    void feed(size_t level, uint64_t row_index, const uint8_t* row) {
        sinks[level]->feed(row);
        if (level + 1 == sinks.size())
            return;
        const uint64_t w_src = sinks[level]->dim.width;
        auto& p = prev_src[level + 1];
        if (row_index & 1) {
            const uint64_t w_dst = sinks[level + 1]->dim.width;
            std::vector<uint8_t> dst(static_cast<size_t>(w_dst) * 3);
            for (uint64_t x = 0; x < w_dst; ++x) {
                const uint64_t xa = 2 * x, xb = std::min(2 * x + 1, w_src - 1);
                for (int c = 0; c < 3; ++c)
                    dst[x * 3 + c] = static_cast<uint8_t>(
                        (static_cast<uint32_t>(row[xa * 3 + c]) + row[xb * 3 + c] +
                            p[xa * 3 + c] + p[xb * 3 + c] + 2) /
                        4);
            }
            feed(level + 1, row_index >> 1, dst.data());
        }
        if (p.size() != static_cast<size_t>(w_src) * 3)
            p.resize(static_cast<size_t>(w_src) * 3);
        std::memcpy(p.data(), row, static_cast<size_t>(w_src) * 3);
    }

    void finish() {
        for (size_t L = 0; L + 1 < sinks.size(); ++L) {
            const uint64_t fed = sinks[L]->rows_fed;
            if (fed & 1) {
                const uint64_t w_src = sinks[L]->dim.width;
                const uint64_t w_dst = sinks[L + 1]->dim.width;
                const auto& p = prev_src[L + 1];
                std::vector<uint8_t> dst(static_cast<size_t>(w_dst) * 3);
                for (uint64_t x = 0; x < w_dst; ++x) {
                    const uint64_t xa = std::min(2 * x, w_src - 1);
                    const uint64_t xb = std::min(2 * x + 1, w_src - 1);
                    for (int c = 0; c < 3; ++c)
                        dst[x * 3 + c] = static_cast<uint8_t>(
                            (static_cast<uint32_t>(p[xa * 3 + c]) + p[xb * 3 + c] + 1) / 2);
                }
                feed(L + 1, fed >> 1, dst.data());
            }
        }
        for (auto* s : sinks)
            s->flush(true);
    }
};

}  // namespace

bool load_job(const std::filesystem::path& manifest, Job& job, std::string& err) {
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        err = "cannot open " + manifest.string();
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string parse_err;
    auto root = json_parse(ss.str(), parse_err);
    if (!root) {
        err = manifest.string() + ": " + parse_err;
        return false;
    }
    const auto* ver = root->find("lumenJob");
    if (!ver || ver->kind != JsonValue::Kind::Number || static_cast<int>(ver->number) != 1) {
        err = "not a lumenJob v1 manifest";
        return false;
    }
    const auto* master = root->find("master");
    const auto scale = json_u32(root->find("scale"));
    if (!master || !scale || !*scale) {
        err = "missing master/scale";
        return false;
    }
    auto y = json_num(master->find("yaw"));
    auto p = json_num(master->find("pitch"));
    auto f = json_num(master->find("fovy"));
    auto vw = json_u32(master->find("vw"));
    auto vh = json_u32(master->find("vh"));
    auto mw = json_u32(master->find("width"));
    auto mh = json_u32(master->find("height"));
    if (!y || !p || !f || !vw || !vh || !mw || !mh) {
        err = "malformed master block";
        return false;
    }
    job.version = 1;
    job.scale = *scale;
    job.master.yaw = *y;
    job.master.pitch = *p;
    job.master.fovy = *f;
    job.master.vw = *vw;
    job.master.vh = *vh;
    job.master.width = *mw;
    job.master.height = *mh;
    job.base_dir = manifest.parent_path();

    const auto* tiles = root->find("tiles");
    const auto* arr = tiles ? tiles->as_array() : nullptr;
    if (!arr || arr->empty()) {
        err = "missing tiles";
        return false;
    }
    for (const auto& t : *arr) {
        JobTile jt;
        const auto* fs = t.find("file");
        if (fs)
            jt.file = fs->as_string() ? *fs->as_string() : std::string();
        auto ty = json_num(t.find("row"));
        auto tx = json_num(t.find("col"));
        auto yaw = json_num(t.find("yaw"));
        auto pitch = json_num(t.find("pitch"));
        auto fov = json_num(t.find("fovy"));
        auto tw = json_u32(t.find("width"));
        auto th = json_u32(t.find("height"));
        const auto* rect = t.find("rect");
        const auto* rects = rect ? rect->as_array() : nullptr;
        if (jt.file.empty() || !ty || !tx || !yaw || !pitch || !fov || !tw || !th || !rects ||
            rects->size() != 4) {
            err = "malformed tile record";
            return false;
        }
        jt.row = static_cast<int>(*ty);
        jt.col = static_cast<int>(*tx);
        jt.yaw = *yaw;
        jt.pitch = *pitch;
        jt.fovy = *fov;
        jt.w = *tw;
        jt.h = *th;
        for (int i = 0; i < 4; ++i)
            jt.rect[i] = static_cast<uint32_t>((*rects)[i].number);
        job.tiles.push_back(std::move(jt));
    }
    return true;
}

namespace {

// Validate master/tile sizes and lay the job tiles into a tilesX x tilesY
// grid of pointers (nulls where a tile is missing).
bool build_grid(const Job& job, std::vector<const JobTile*>& grid, uint32_t& tilesX,
    uint32_t& tilesY, std::string& err) {
    const uint32_t vw = job.master.vw, vh = job.master.vh;
    const uint32_t W = job.master.width, H = job.master.height;
    if (!vw || !vh || !W || !H) {
        err = "zero-size master";
        return false;
    }
    tilesX = W / vw, tilesY = H / vh;
    if (tilesX * vw != W || tilesY * vh != H) {
        err = "master size is not an integer multiple of the tile size";
        return false;
    }
    grid.assign(static_cast<size_t>(tilesX) * tilesY, nullptr);
    for (const auto& t : job.tiles) {
        if (t.row < 0 || t.row >= static_cast<int>(tilesY) || t.col < 0 ||
            t.col >= static_cast<int>(tilesX)) {
            err = "tile outside the grid";
            return false;
        }
        grid[static_cast<size_t>(t.row) * tilesX + t.col] = &t;
    }
    for (size_t i = 0; i < grid.size(); ++i) {
        if (!grid[i]) {
            err = "tile grid has holes - capture incomplete or tiles went missing";
            return false;
        }
    }
    return true;
}

}  // namespace

bool composite(const Job& job, const StripFn& strip,
    const std::function<void(uint32_t percent)>& progress, std::string& err) {
    const uint32_t vw = job.master.vw, vh = job.master.vh;
    const uint32_t W = job.master.width, H = job.master.height;
    std::vector<const JobTile*> grid;
    uint32_t tilesX = 0, tilesY = 0;
    if (!build_grid(job, grid, tilesX, tilesY, err))
        return false;

    std::vector<uint8_t> out_row(static_cast<size_t>(W) * 3);
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
        // Decode this band's tile row (freed at the band's end).
        std::vector<std::vector<uint8_t>> imgs(tilesX);
        std::vector<uint32_t> iw(tilesX, 0), ih(tilesX, 0);
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            const JobTile& t = *grid[static_cast<size_t>(ty) * tilesX + tx];
            const auto path = job.base_dir / t.file;
            int tw = 0, th = 0, ch = 0;
            unsigned char* px = stbi_load(path.string().c_str(), &tw, &th, &ch, 4);
            if (!px) {
                err = "cannot decode tile " + path.string();
                return false;
            }
            iw[tx] = static_cast<uint32_t>(tw);
            ih[tx] = static_cast<uint32_t>(th);
            imgs[tx].assign(px, px + static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);
            stbi_image_free(px);
        }
        std::vector<tilemath::RayMap> maps(tilesX);
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            const JobTile& t = *grid[static_cast<size_t>(ty) * tilesX + tx];
            tilemath::Tile tm;
            tm.yaw = t.yaw;
            tm.pitch = t.pitch;
            tm.fovy = t.fovy;
            tm.w = t.w;
            tm.h = t.h;
            maps[tx] = tilemath::RayMap(job.master, tm);
        }
        const uint64_t y_lo = static_cast<uint64_t>(ty) * vh;
        const uint64_t y_hi = std::min<uint64_t>(y_lo + vh, H);
        for (uint64_t y = y_lo; y < y_hi; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const uint32_t tx = std::min(x / vw, tilesX - 1);
                double px, py;
                uint8_t* dst = out_row.data() + static_cast<size_t>(x) * 3;
                if (!maps[tx].eval(x, y, px, py)) {
                    dst[0] = dst[1] = dst[2] = 0;
                    continue;
                }
                const std::vector<uint8_t>& img = imgs[tx];
                sample_tile(img.data(), iw[tx], ih[tx], px, py, dst);
            }
            if (!strip(y, 1, out_row.data())) {
                err = "strip consumer failed";
                return false;
            }
        }
        if (progress)
            progress(static_cast<uint32_t>(100ull * y_hi / H));
    }
    return true;
}

bool composite_scaled(const Job& job, uint32_t factor, const StripFn& strip,
    const std::function<void(uint32_t percent)>& progress, std::string& err) {
    if (factor == 0)
        factor = 1;
    const uint32_t vw = job.master.vw, vh = job.master.vh;
    const uint32_t W = job.master.width, H = job.master.height;
    std::vector<const JobTile*> grid;
    uint32_t tilesX = 0, tilesY = 0;
    if (!build_grid(job, grid, tilesX, tilesY, err))
        return false;
    const uint64_t OW = (W + factor - 1ull) / factor, OH = (H + factor - 1ull) / factor;

    // Output rows advance monotonically, so tile bands (grid rows) are
    // decoded on demand into a sliding window and evicted behind us. An
    // output row spanning several bands keeps them all live - unavoidable,
    // its samples genuinely come from each.
    struct Band {
        std::vector<std::vector<uint8_t>> imgs;
        std::vector<uint32_t> iw, ih;
        std::vector<tilemath::RayMap> maps;
    };
    std::deque<Band> cache;
    uint32_t cache_first = 0;
    const auto decode_band = [&](uint32_t ty) -> bool {
        Band b;
        b.imgs.resize(tilesX);
        b.iw.resize(tilesX, 0);
        b.ih.resize(tilesX, 0);
        b.maps.resize(tilesX);
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            const JobTile& t = *grid[static_cast<size_t>(ty) * tilesX + tx];
            const auto path = job.base_dir / t.file;
            int tw = 0, th = 0, ch = 0;
            unsigned char* px = stbi_load(path.string().c_str(), &tw, &th, &ch, 4);
            if (!px) {
                err = "cannot decode tile " + path.string();
                return false;
            }
            b.iw[tx] = static_cast<uint32_t>(tw);
            b.ih[tx] = static_cast<uint32_t>(th);
            b.imgs[tx].assign(px, px + static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);
            stbi_image_free(px);
            tilemath::Tile tm;
            tm.yaw = t.yaw;
            tm.pitch = t.pitch;
            tm.fovy = t.fovy;
            tm.w = t.w;
            tm.h = t.h;
            b.maps[tx] = tilemath::RayMap(job.master, tm);
        }
        cache.push_back(std::move(b));
        return true;
    };

    std::vector<uint8_t> out_row(static_cast<size_t>(OW) * 3);
    for (uint64_t y = 0; y < OH; ++y) {
        const uint64_t my0 = y * factor;
        const uint64_t my1 = std::min((y + 1) * factor, static_cast<uint64_t>(H)) - 1;
        const uint32_t b0 = static_cast<uint32_t>(my0 / vh);
        const uint32_t b1 = static_cast<uint32_t>(my1 / vh);
        while (cache_first < b0) {
            cache.pop_front();
            ++cache_first;
        }
        while (cache_first + cache.size() <= b1) {
            if (!decode_band(static_cast<uint32_t>(cache_first + cache.size())))
                return false;
        }
        for (uint64_t x = 0; x < OW; ++x) {
            const uint64_t mx0 = x * factor;
            const uint64_t mx1 = std::min((x + 1) * factor, static_cast<uint64_t>(W)) - 1;
            uint64_t acc[3] = {0, 0, 0};
            uint32_t n = 0;
            for (uint64_t my = my0; my <= my1; ++my) {
                Band& band = cache[static_cast<size_t>((my / vh) - cache_first)];
                for (uint64_t mx = mx0; mx <= mx1; ++mx) {
                    const uint32_t tx =
                        static_cast<uint32_t>(std::min(mx / vw, static_cast<uint64_t>(tilesX - 1)));
                    double px, py;
                    if (!band.maps[tx].eval(static_cast<double>(mx), static_cast<double>(my), px,
                            py))
                        continue;  // outside this tile's footprint (master border only)
                    uint8_t s[3];
                    sample_tile(band.imgs[tx].data(), band.iw[tx], band.ih[tx], px, py, s);
                    acc[0] += s[0];
                    acc[1] += s[1];
                    acc[2] += s[2];
                    ++n;
                }
            }
            uint8_t* dst = out_row.data() + static_cast<size_t>(x) * 3;
            if (n) {
                for (int c = 0; c < 3; ++c)
                    dst[c] = static_cast<uint8_t>((acc[c] + n / 2) / n);
            } else
                dst[0] = dst[1] = dst[2] = 0;
        }
        if (!strip(y, 1, out_row.data())) {
            err = "strip consumer failed";
            return false;
        }
        if (progress)
            progress(static_cast<uint32_t>(100ull * (y + 1) / OH));
    }
    return true;
}

bool run(const std::filesystem::path& manifest, const Options& opt, std::string& err,
    const std::function<void(uint32_t percent)>& progress) {
    Job job;
    if (!load_job(manifest, job, err))
        return false;

    const uint32_t dscale = opt.downscale ? opt.downscale : 1;
    std::vector<BigTiffWriter::Level> levels;
    {
        BigTiffWriter::Level l0;
        l0.width = (job.master.width + dscale - 1) / dscale;
        l0.height = (job.master.height + dscale - 1) / dscale;
        l0.tileW = l0.tileH = 256;
        levels.push_back(l0);
        while (std::max(levels.back().width, levels.back().height) > opt.min_mip_edge) {
            BigTiffWriter::Level l;
            l.width = (levels.back().width + 1) / 2;
            l.height = (levels.back().height + 1) / 2;
            l.tileW = l.tileH = 256;
            levels.push_back(l);
        }
    }

    auto out_path = opt.out.empty() ? std::filesystem::path(manifest).replace_extension(".tiff")
                                    : opt.out;
    BigTiffWriter tiff;
    if (!tiff.open(out_path, levels, err))
        return false;

    std::vector<LevelSink> sinks(levels.size());
    std::vector<uint8_t> preview_raster;
    size_t preview_level = levels.size();
    for (size_t l = 0; l < levels.size(); ++l) {
        sinks[l].dim = levels[l];
        sinks[l].tiff = &tiff;
        sinks[l].level_index = l;
        if ((opt.preview_jpeg || opt.preview_png) && preview_level == levels.size() &&
            std::max(levels[l].width, levels[l].height) <= opt.preview_max) {
            preview_level = l;
            sinks[l].preview = &preview_raster;
        }
    }
    MipFeeder feeder;
    for (auto& s : sinks)
        feeder.sinks.push_back(&s);
    feeder.prev_src.resize(levels.size());

    const auto sink_rows = [&](uint64_t y, uint64_t rows, const uint8_t* rgb) {
        for (uint64_t r = 0; r < rows; ++r)
            feeder.feed(0, y + r, rgb + static_cast<size_t>(r) * levels[0].width * 3);
        return true;
    };
    const bool ok = dscale == 1 ? composite(job, sink_rows, progress, err)
                                : composite_scaled(job, dscale, sink_rows, progress, err);
    feeder.finish();
    if (!tiff.close(err))
        return false;
    if (!ok)
        return false;

    if (preview_level < levels.size() && !preview_raster.empty()) {
        const auto dir = opt.preview_dir.empty() ? out_path.parent_path() : opt.preview_dir;
        const auto stem = out_path.stem().string();
        const uint32_t pw = static_cast<uint32_t>(levels[preview_level].width);
        const uint32_t ph = static_cast<uint32_t>(levels[preview_level].height);
        if (preview_raster.size() < static_cast<size_t>(pw) * ph * 3) {
            err = "preview raster incomplete";
            return false;
        }
        if (opt.preview_jpeg)
            stbi_write_jpg((dir / (stem + ".jpg")).string().c_str(), static_cast<int>(pw),
                static_cast<int>(ph), 3, preview_raster.data(), 92);
        if (opt.preview_png)
            stbi_write_png((dir / (stem + ".png")).string().c_str(), static_cast<int>(pw),
                static_cast<int>(ph), 3, preview_raster.data(), static_cast<int>(pw) * 3);
    }
    return true;
}

}  // namespace lumen::stitch
