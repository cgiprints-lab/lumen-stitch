// Minimal tiled BigTIFF writer (little-endian, uncompressed, 8-bit RGB).
//
// Deliberately dependency-free: gigapixel stitchers elsewhere pull in libtiff;
// we write the ~250 bytes of structure ourselves so `lumen` ships as one
// static binary. Output is a standard BigTIFF (version 43, 64-bit offsets),
// tiled for pan/zoom friendliness, with a multiresolution SubIFD chain
// (level 0 primary, levels 1.. reduced), readable by libtiff, ImageMagick,
// QuPath, affy, web viewers, etc.
//
// Usage: open(levels), stream tiles per level in raster tile order via
// writeTile(), then close() which lays down the tables, IFD chain and patches
// the header. Tiles can therefore stream with zero seeks.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace lumen::stitch {

class BigTiffWriter {
public:
    struct Level {
        uint64_t width = 0, height = 0;
        uint32_t tileW = 256, tileH = 256;
    };

    ~BigTiffWriter();

    bool open(const std::filesystem::path& path, const std::vector<Level>& levels, std::string& err);

    // One tile in raster tile order for `level`; nbytes = croppedW * croppedH * spp.
    void writeTile(size_t level, const uint8_t* rgb, uint64_t nbytes);

    bool close(std::string& err);

    uint64_t tilesWritten(size_t level) const { return offsets_[level].size(); }
    uint64_t tilesExpected(size_t level) const;

private:
    static void put16(std::FILE*, uint16_t);
    static void put64(std::FILE*, uint64_t);

    std::FILE* f_ = nullptr;
    std::vector<Level> levels_;
    uint16_t spp_ = 3;
    std::vector<std::vector<uint64_t>> offsets_, counts_;
    uint64_t subifd_array_pos_ = 0;  // reserved region for IFD0 SubIFDs values
};

}  // namespace lumen::stitch
