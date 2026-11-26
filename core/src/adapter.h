// Adapter manifests: adapters/<id>/adapter.json
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct Adapter {
    std::string id;
    std::string name;
    std::filesystem::path dir;  // adapters/<id>

    // detection
    std::vector<std::string> data_dir_tokens;
    std::string mod_subdir = "mods";
    std::vector<std::string> markers;

    // integration
    std::string mod_id;
    std::string bundle = "integration.dusk";
    std::string source_dir;      // relative to adapter dir, empty => no source build
    std::string cmake_target;    // target to build inside source_dir

    // artifact paths relative to adapter dir, keyed by platform slug
    std::vector<std::pair<std::string, std::string>> artifacts;

    static std::optional<Adapter> load(const std::filesystem::path& dir,
                                       std::string& error);
};

std::vector<Adapter> load_adapters(const std::filesystem::path& adapters_dir);

// Current platform slug: windows-x64 | linux-x64 | macos-arm64 | android-arm64
std::string platform_slug();

}  // namespace lumen
