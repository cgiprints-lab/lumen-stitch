#include "bigtiff.h"

#include <cstring>

namespace lumen::stitch {

namespace {
// TIFF tag/type constants we use.
constexpr uint16_t kTagNewSubfileType = 254;
constexpr uint16_t kTagImageWidth = 256;
constexpr uint16_t kTagImageLength = 257;
constexpr uint16_t kTagBitsPerSample = 258;
constexpr uint16_t kTagCompression = 259;
constexpr uint16_t kTagPhotometric = 262;
constexpr uint16_t kTagSamplesPerPixel = 277;
constexpr uint16_t kTagPlanarConfig = 284;
constexpr uint16_t kTagSoftware = 305;
constexpr uint16_t kTagTileWidth = 322;
constexpr uint16_t kTagTileLength = 323;
constexpr uint16_t kTagTileOffsets = 324;
constexpr uint16_t kTagTileByteCounts = 325;
// Standard SubIFD tag (LibTIFF, GDAL, Photoshop pyramid readers all key on
// 330). An early build used a private tag (33005) which spec-compliant
// readers ignore; do not change it back.
constexpr uint16_t kTagSubIFDs = 330;

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeAscii = 2;
constexpr uint16_t kTypeLong8 = 16;

constexpr const char* kSoftware = "Lumen Gigapixel Stitcher";
}  // namespace

BigTiffWriter::~BigTiffWriter() {
    if (f_)
        std::fclose(f_);
}

void BigTiffWriter::put16(std::FILE* f, uint16_t v) {
    std::fputc(v & 0xFF, f);
    std::fputc((v >> 8) & 0xFF, f);
}

void BigTiffWriter::put64(std::FILE* f, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        std::fputc((v >> (8 * i)) & 0xFF, f);
}

bool BigTiffWriter::open(const std::filesystem::path& path, const std::vector<Level>& levels,
    std::string& err) {
    if (levels.empty()) {
        err = "no levels";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    f_ = std::fopen(path.string().c_str(), "wb");
    if (!f_) {
        err = "cannot open " + path.string();
        return false;
    }
    levels_ = levels;
    offsets_.assign(levels_.size(), {});
    counts_.assign(levels_.size(), {});

    // Header: II, version 43, offset size 8, placeholder IFD0 offset.
    put16(f_, 0x4949);
    put16(f_, 43);
    put16(f_, 8);
    put16(f_, 0);
    put64(f_, 0);
    // Reserve the SubIFD value array (levels-1 pointers); patched in close().
    subifd_array_pos_ = std::ftell(f_);
    for (size_t i = 1; i < levels_.size(); ++i)
        put64(f_, 0);
    return true;
}

uint64_t BigTiffWriter::tilesExpected(size_t level) const {
    const Level& L = levels_[level];
    const uint64_t tx = (L.width + L.tileW - 1) / L.tileW;
    const uint64_t ty = (L.height + L.tileH - 1) / L.tileH;
    return tx * ty;
}

void BigTiffWriter::writeTile(size_t level, const uint8_t* rgb, uint64_t nbytes) {
    offsets_[level].push_back(static_cast<uint64_t>(std::ftell(f_)));
    counts_[level].push_back(nbytes);
    std::fwrite(rgb, 1, nbytes, f_);
}

bool BigTiffWriter::close(std::string& err) {
    if (!f_) {
        err = "not open";
        return false;
    }
    const size_t n = levels_.size();
    std::vector<uint64_t> ifd_offsets(n, 0);
    std::vector<uint64_t> tile_offsets_pos(n, 0), tile_counts_pos(n, 0), software_pos(n, 0);

    // Per-level tile tables.
    for (size_t l = 0; l < n; ++l) {
        tile_offsets_pos[l] = static_cast<uint64_t>(std::ftell(f_));
        for (uint64_t v : offsets_[l])
            put64(f_, v);
        tile_counts_pos[l] = static_cast<uint64_t>(std::ftell(f_));
        for (uint64_t v : counts_[l])
            put64(f_, v);
        software_pos[l] = static_cast<uint64_t>(std::ftell(f_));
        std::fwrite(kSoftware, 1, std::strlen(kSoftware) + 1, f_);
    }

    auto entry = [&](uint16_t tag, uint16_t type, uint64_t count, uint64_t value) {
        put16(f_, tag);
        put16(f_, type);
        put64(f_, count);
        put64(f_, value);
    };

    // SHORT array entry inlined in the 8-byte value field (spp <= 4 shorts
    // fit inline in BigTIFF; our streams are RGB).
    auto entry_bits = [&](uint16_t tag, uint64_t count, uint16_t v) {
        put16(f_, tag);
        put16(f_, kTypeShort);
        put64(f_, count);
        uint64_t packed = 0;
        for (uint64_t i = 0; i < count && i < 4; ++i)
            packed |= static_cast<uint64_t>(v) << (16 * i);
        put64(f_, packed);
    };

    for (size_t l = 0; l < n; ++l) {
        const Level& L = levels_[l];
        ifd_offsets[l] = static_cast<uint64_t>(std::ftell(f_));
        size_t entries = 14;
        if (l != 0)
            entries -= 1;  // no SubIFDs on reduced levels
        // BigTIFF IFD entry count is a uint64 (classic TIFF uses uint16 -
        // writing it short misaligns the whole IFD and readers report an
        // unexpected EOF; the seam test's reader once mirrored the bug).
        put64(f_, entries);
        entry(kTagNewSubfileType, kTypeShort, 1, l == 0 ? 0 : 1);
        entry(kTagImageWidth, kTypeLong8, 1, L.width);
        entry(kTagImageLength, kTypeLong8, 1, L.height);
        // Classic TIFF requires one BitsPerSample value PER SAMPLE for RGB.
        entry_bits(kTagBitsPerSample, spp_, 8);
        entry(kTagCompression, kTypeShort, 1, 1);  // none
        entry(kTagPhotometric, kTypeShort, 1, 2);  // RGB
        entry(kTagSamplesPerPixel, kTypeShort, 1, spp_);
        entry(kTagPlanarConfig, kTypeShort, 1, 1);
        // TIFF mandates entries in ascending tag order - Photoshop's reader
        // stops at the first out-of-order entry and loses the tile tags
        // (renders the image black), so Software must precede the tile tags.
        entry(kTagSoftware, kTypeAscii, std::strlen(kSoftware) + 1, software_pos[l]);
        entry(kTagTileWidth, kTypeLong8, 1, L.tileW);
        entry(kTagTileLength, kTypeLong8, 1, L.tileH);
        // A single LONG8 array element (<= 8 bytes) must be stored INLINE in
        // the value field, not as a pointer - readers key on byte count, and
        // deep pyramid levels have exactly one tile.
        const uint64_t nt = offsets_[l].size();
        entry(kTagTileOffsets, kTypeLong8, nt, nt == 1 ? offsets_[l][0] : tile_offsets_pos[l]);
        entry(kTagTileByteCounts, kTypeLong8, nt, nt == 1 ? counts_[l][0] : tile_counts_pos[l]);
        if (l == 0)
            entry(kTagSubIFDs, kTypeLong8, n - 1, subifd_array_pos_);
        put64(f_, 0);  // next IFD: chain intentionally unlinked (SubIFDs carry mips)
    }

    // Patch SubIFD array.
    std::fseek(f_, static_cast<long>(subifd_array_pos_), SEEK_SET);
    for (size_t l = 1; l < n; ++l)
        put64(f_, ifd_offsets[l]);

    // Patch header IFD0 offset.
    std::fseek(f_, 8, SEEK_SET);
    put64(f_, ifd_offsets[0]);

    if (std::fflush(f_) != 0 || std::ferror(f_)) {
        err = "write error";
        std::fclose(f_);
        f_ = nullptr;
        return false;
    }
    std::fclose(f_);
    f_ = nullptr;
    return true;
}

}  // namespace lumen::stitch
