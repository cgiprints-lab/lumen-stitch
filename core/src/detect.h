// Detects game installs from adapter manifests.
#pragma once

#include "adapter.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lumen {

struct Install {
    std::filesystem::path data_dir;  // e.g. %APPDATA%/TwilitRealm/Dusklight
    std::filesystem::path mod_dir;   // data_dir / mod_subdir

    std::filesystem::path bundle_path(const Adapter& a) const {
        return mod_dir / a.bundle;
    }
};

std::vector<Install> detect_installs(const Adapter& adapter);

}  // namespace lumen
