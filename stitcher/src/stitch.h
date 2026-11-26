// Gigapixel stitcher: composites a Lumen tile job into a tiled multiresolution
// BigTIFF without ever holding the full image (strip-composited, tile rows
// decoded per band and freed), plus optional reduced-level previews.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "tilemath.h"

namespace lumen::stitch {

struct JobTile {
    std::string file;
    int row = 0, col = 0;
    double yaw = 0;    // radians
    double pitch = 0;  // radians
    double fovy = 90;  // degrees
    uint32_t w = 0, h = 0;
    uint32_t rect[4] = {0, 0, 0, 0};  // master-pixel rect [x0,y0,x1,y1)
};

struct Job {
    int version = 1;
    uint32_t scale = 1;
    tilemath::Master master;
    std::vector<JobTile> tiles;
    std::filesystem::path base_dir;  // tile files resolved against this
};

bool load_job(const std::filesystem::path& manifest, Job& job, std::string& err);

// Receives composite output: `rows` master rows starting at y0, RGB packed,
// row stride = master.width * 3.
using StripFn = std::function<bool(uint64_t y0, uint64_t rows, const uint8_t* rgb)>;

// Composite the full master grid through `strip`.
bool composite(const Job& job, const StripFn& strip,
    const std::function<void(uint32_t percent)>& progress, std::string& err);

struct Options {
    std::filesystem::path out;              // primary .tiff
    bool preview_jpeg = true;               // reduced-level JPEG preview
    bool preview_png = false;               // reduced-level PNG preview
    uint32_t preview_max = 4096;            // longest edge for previews
    uint32_t min_mip_edge = 16;             // stop mip chain below this edge
    std::filesystem::path preview_dir;      // default: alongside `out`
};

bool run(const std::filesystem::path& manifest, const Options& opt, std::string& err,
    const std::function<void(uint32_t percent)>& progress = {});

}  // namespace lumen::stitch
