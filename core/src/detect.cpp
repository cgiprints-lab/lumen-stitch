#include "detect.h"

#include "util.h"

namespace lumen {

namespace {

bool marker_hit(const std::filesystem::path& root, const DetectLayout& layout,
                std::error_code& ec) {
    if (layout.markers.empty())
        return true;
    if (layout.markers_all) {
        for (const auto& m : layout.markers)
            if (!std::filesystem::exists(root / m, ec))
                return false;
        return true;
    }
    for (const auto& m : layout.markers)
        if (std::filesystem::exists(root / m, ec))
            return true;
    return false;
}

void add_layout(std::vector<DetectLayout>& out, const Adapter& a) {
    if (!a.layouts.empty()) {
        out = a.layouts;
        return;
    }
    DetectLayout flat;  // adapters written before "layouts"
    flat.dir_tokens = a.data_dir_tokens;
    flat.markers = a.markers;
    flat.mod_subdir = a.mod_subdir;
    out.push_back(std::move(flat));
}

}  // namespace

std::vector<Install> detect_installs(const Adapter& adapter) {
    std::vector<Install> found;
    std::error_code ec;
    std::vector<DetectLayout> layouts;
    add_layout(layouts, adapter);

    for (const auto& layout : layouts) {
        for (const auto& token : layout.dir_tokens) {
            std::filesystem::path base = expand_path(token);
            if (base.empty() || !std::filesystem::is_directory(base, ec))
                continue;
            std::vector<std::filesystem::path> roots{base};
            if (layout.scan_subdirs) {
                // A launcher keeps one folder per profile (or per version),
                // named by the player: each child is a root of its own.
                for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
                    if (entry.is_directory())
                        roots.push_back(entry.path());
                }
            }
            for (const auto& root : roots) {
                if (!marker_hit(root, layout, ec))
                    continue;
                bool seen = false;
                for (const auto& prev : found) {
                    if (prev.data_dir == root) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;
                Install in;
                in.data_dir = root;
                in.mod_dir = root / layout.mod_subdir;
                found.push_back(std::move(in));
            }
        }
    }
    return found;
}

}  // namespace lumen
