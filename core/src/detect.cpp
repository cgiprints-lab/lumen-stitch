#include "detect.h"

#include "util.h"

namespace lumen {

std::vector<Install> detect_installs(const Adapter& adapter) {
    std::vector<Install> found;
    std::error_code ec;
    for (const auto& token : adapter.data_dir_tokens) {
        std::filesystem::path data = expand_path(token);
        if (data.empty())
            continue;
        if (!std::filesystem::is_directory(data, ec))
            continue;
        bool marker_ok = adapter.markers.empty();
        for (const auto& m : adapter.markers) {
            if (std::filesystem::exists(data / m, ec)) {
                marker_ok = true;
                break;
            }
        }
        if (!marker_ok)
            continue;
        Install in;
        in.data_dir = data;
        in.mod_dir = data / adapter.mod_subdir;
        found.push_back(std::move(in));
    }
    return found;
}

}  // namespace lumen
