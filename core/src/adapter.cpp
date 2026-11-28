#include "adapter.h"

#include "json.h"
#include "util.h"

#include <fstream>

namespace lumen {

std::optional<Adapter> Adapter::load(const std::filesystem::path& dir, std::string& error) {
    std::filesystem::path manifest = dir / "adapter.json";
    bool ok = false;
    std::string text = read_file_string(manifest, ok);
    if (!ok) {
        error = "cannot read " + manifest.string();
        return std::nullopt;
    }
    JsonValue root = json_parse(text, error).value_or(JsonValue{});
    if (!root.is_object()) {
        error = manifest.string() + ": expected a JSON object";
        return std::nullopt;
    }
    Adapter a;
    a.dir = dir;
    if (auto v = root.find("id"); v && v->is_string())
        a.id = v->string;
    if (auto v = root.find("name"); v && v->is_string())
        a.name = v->string;
    if (a.id.empty()) {
        error = manifest.string() + ": missing \"id\"";
        return std::nullopt;
    }
    if (a.name.empty())
        a.name = a.id;

    if (auto det = root.find("detection"); det && det->is_object()) {
        if (auto dirs = det->find("data_dirs"); dirs && dirs->is_array())
            for (const auto& d : dirs->array)
                if (d.is_string())
                    a.data_dir_tokens.push_back(d.string);
        if (auto v = det->find("mod_subdir"); v && v->is_string())
            a.mod_subdir = v->string;
        if (auto v = det->find("markers"); v && v->is_array())
            for (const auto& m : v->array)
                if (m.is_string())
                    a.markers.push_back(m.string);
        // layouts: several folder shapes one adapter can live in.
        if (auto ls = det->find("layouts"); ls && ls->is_array()) {
            for (const auto& l : ls->array) {
                if (!l.is_object())
                    continue;
                DetectLayout lay;
                if (auto dirs = l.find("dirs"); dirs && dirs->is_array())
                    for (const auto& d : dirs->array)
                        if (d.is_string())
                            lay.dir_tokens.push_back(d.string);
                if (auto ms = l.find("markers"); ms && ms->is_array())
                    for (const auto& m : ms->array)
                        if (m.is_string())
                            lay.markers.push_back(m.string);
                if (auto v = l.find("markers_all"); v && v->is_bool())
                    lay.markers_all = v->boolean;
                if (auto v = l.find("mod_subdir"); v && v->is_string())
                    lay.mod_subdir = v->string;
                if (auto v = l.find("scan_subdirs"); v && v->is_bool())
                    lay.scan_subdirs = v->boolean;
                if (!lay.dir_tokens.empty())
                    a.layouts.push_back(std::move(lay));
            }
        }
    }
    if (auto integ = root.find("integration"); integ && integ->is_object()) {
        if (auto v = integ->find("mod_id"); v && v->is_string())
            a.mod_id = v->string;
        if (auto v = integ->find("bundle"); v && v->is_string())
            a.bundle = v->string;
        if (auto v = integ->find("source_dir"); v && v->is_string())
            a.source_dir = v->string;
        if (auto v = integ->find("cmake_target"); v && v->is_string())
            a.cmake_target = v->string;
    }
    if (auto arts = root.find("artifacts"); arts && arts->is_object())
        for (const auto& [k, v] : arts->object)
            if (v.is_string())
                a.artifacts.emplace_back(k, v.string);

    return a;
}

std::vector<Adapter> load_adapters(const std::filesystem::path& adapters_dir) {
    std::vector<Adapter> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(adapters_dir, ec))
        return out;
    for (const auto& entry : std::filesystem::directory_iterator(adapters_dir, ec)) {
        if (!entry.is_directory())
            continue;
        std::string error;
        if (auto a = Adapter::load(entry.path(), error))
            out.push_back(std::move(*a));
    }
    return out;
}

std::string platform_slug() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__ANDROID__)
    return "android-arm64";
#elif defined(__APPLE__)
    return "macos-arm64";
#elif defined(__linux__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

}  // namespace lumen
