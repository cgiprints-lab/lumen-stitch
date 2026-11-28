// Adapter manifests: adapters/<id>/adapter.json
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// One detection layout: a set of roots, the marker that says "a game lives
// here", and where mods go under that root. Games change their folder shape
// between releases (Longshot #2286 moved mods from <game>/profile/mods to a
// profile folder of its own), so an adapter may name several, tried in order.
struct DetectLayout {
    std::vector<std::string> dir_tokens;     // expanded with expand_path()
    std::vector<std::string> markers;        // mark a root...
    // False (default): any one marker is enough. True: every marker must be
    // there - how an older folder shape is told from a newer one that shares
    // a marker file (Longshot's game folders moved image.bin into data/).
    bool markers_all = false;
    std::string mod_subdir = "mods";         // relative to the matched root
    // True: every immediate subdirectory of a token is itself a root to
    // check (a launcher keeps one folder per profile, named by the player).
    bool scan_subdirs = false;
};

struct Adapter {
    std::string id;
    std::string name;
    std::filesystem::path dir;  // adapters/<id>

    // detection: `layouts` when the manifest names several; otherwise the
    // single flat layout below (kept for adapters written before layouts).
    std::vector<DetectLayout> layouts;
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
