// Lumen Manager - standalone installer for Ansel-style game integrations.
//
//   lumen status
//   lumen install --adapter <id> [--artifact <path> | --build] [--all]
//   lumen uninstall --adapter <id> [--all]
#include "adapter.h"
#include "detect.h"
#include "state.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#ifndef LUMEN_SOURCE_DIR
#define LUMEN_SOURCE_DIR "."
#endif

namespace {

using namespace lumen;

std::filesystem::path adapters_dir(int argc, char** argv) {
    if (auto v = arg_value(argc, argv, "--adapters"))
        return std::filesystem::path(*v);
    // Prefer adapters next to the executable, fall back to the source tree.
    std::error_code ec;
    std::filesystem::path exe_dir = std::filesystem::weakly_canonical(
            std::filesystem::path(std::filesystem::absolute(
                    std::filesystem::path(argv[0]))).parent_path(), ec);
    for (const auto& candidate :
         {exe_dir / "adapters", exe_dir / ".." / "adapters",
          std::filesystem::path(LUMEN_SOURCE_DIR) / "adapters"}) {
        if (std::filesystem::is_directory(candidate, ec))
            return candidate.lexically_normal();
    }
    return std::filesystem::path(LUMEN_SOURCE_DIR) / "adapters";
}

std::vector<Adapter> select_adapters(int argc, char** argv,
                                     const std::filesystem::path& dir) {
    auto all = load_adapters(dir);
    auto want = arg_value(argc, argv, "--adapter");
    if (!want)
        return all;
    std::vector<Adapter> out;
    for (auto& a : all)
        if (a.id == *want)
            out.push_back(std::move(a));
    return out;
}

const char* exists_text(bool e) { return e ? "yes" : "no"; }

void print_status(const Adapter& a) {
    auto installs = detect_installs(a);
    std::cout << "adapter " << a.id << " (" << a.name << ")\n";
    if (installs.empty()) {
        std::cout << "  no install detected (looked in:";
        for (const auto& t : a.data_dir_tokens)
            std::cout << " " << t;
        std::cout << ")\n";
        return;
    }
    for (const auto& in : installs) {
        std::error_code ec;
        bool present = std::filesystem::exists(in.bundle_path(a), ec);
        std::cout << "  install: " << in.data_dir.string() << "\n";
        std::cout << "    integration (" << a.bundle << "): " << exists_text(present);
        if (present) {
            auto size = std::filesystem::file_size(in.bundle_path(a), ec);
            std::cout << " (" << size << " bytes)";
        }
        std::cout << "\n";
        auto records = state_find(a, in);
        if (!records.empty()) {
            std::cout << "    installed by lumen: " << records.back().installedAt
                      << " (" << records.back().source << ")\n";
        } else if (!present) {
            std::cout << "    not installed\n";
        }
    }
}

bool run_cmake_build(const Adapter& a, std::filesystem::path& out_bundle) {
    if (a.source_dir.empty()) {
        std::cerr << "adapter " << a.id << " has no source_dir; cannot --build\n";
        return false;
    }
    std::filesystem::path src = a.dir / a.source_dir;
    std::filesystem::path build = src / "build";
    auto configure = [&](const char* extra) {
        std::string cmd = std::string("cmake -S \"") + src.string() + "\" -B \"" +
                          build.string() + "\" -G Ninja" + (extra ? extra : "");
        return std::system(cmd.c_str()) == 0;
    };
    if (!configure("") && !configure(" --fresh")) {
        std::cerr << "cmake configure failed (is cmake on PATH?)\n";
        return false;
    }
    std::string target = a.cmake_target.empty() ? "" : a.cmake_target;
    std::string cmd = "cmake --build \"" + build.string() + "\"";
    if (!target.empty())
        cmd += " --target " + target;
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "cmake build failed\n";
        return false;
    }
    std::error_code ec;
    std::filesystem::path produced = build / "mods" / a.bundle;
    if (!std::filesystem::exists(produced, ec)) {
        std::cerr << "build did not produce " << produced.string() << "\n";
        return false;
    }
    out_bundle = produced;
    return true;
}

bool resolve_artifact(const Adapter& a, int argc, char** argv,
                      std::filesystem::path& out_bundle, std::string& source) {
    if (arg_flag(argc, argv, "--build"))
        return run_cmake_build(a, out_bundle) ? (source = "build", true) : false;
    if (auto v = arg_value(argc, argv, "--artifact")) {
        out_bundle = std::filesystem::path(*v);
        source = "artifact";
        return std::filesystem::exists(out_bundle);
    }
    // manifest artifact for the current platform
    std::string slug = platform_slug();
    for (const auto& [k, p] : a.artifacts) {
        if (k == slug) {
            out_bundle = a.dir / p;
            source = "prebuilt:" + slug;
            if (std::filesystem::exists(out_bundle))
                return true;
            std::cerr << "prebuilt artifact missing: " << out_bundle.string()
                      << "\n  (use --build or --artifact <path>)\n";
            return false;
        }
    }
    std::cerr << "no artifact for platform " << slug << "\n";
    return false;
}

int cmd_install(const Adapter& a, int argc, char** argv) {
    auto installs = detect_installs(a);
    if (installs.empty()) {
        std::cerr << "no " << a.name << " install detected; nothing to do\n";
        return 2;
    }
    std::filesystem::path bundle;
    std::string source;
    if (!resolve_artifact(a, argc, argv, bundle, source)) {
        std::cerr << "cannot resolve integration bundle for " << a.id << "\n";
        return 1;
    }
    bool any = false;
    for (const auto& in : installs) {
        std::error_code ec;
        std::filesystem::create_directories(in.mod_dir, ec);
        std::filesystem::path dest = in.bundle_path(a);
        if (std::filesystem::exists(dest, ec)) {
            auto backup = dest;
            backup += ".bak-" + std::to_string(std::time(nullptr));
            std::filesystem::rename(dest, backup, ec);
        }
        std::filesystem::copy_file(bundle, dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "failed to install to " << dest.string() << ": " << ec.message() << "\n";
            return 1;
        }
        state_record_install(a, in, source);
        std::cout << "installed " << bundle.string() << " -> " << dest.string() << "\n";
        any = true;
        break;  // first detected install
    }
    return any ? 0 : 1;
}

int cmd_uninstall(const Adapter& a, int argc, char** argv) {
    int done = 0;
    for (const auto& in : detect_installs(a)) {
        std::error_code ec;
        if (std::filesystem::remove(in.bundle_path(a), ec)) {
            std::cout << "removed " << in.bundle_path(a).string() << "\n";
            done++;
        }
        state_remove_install(a, in);
        if (!arg_flag(argc, argv, "--all"))
            break;
    }
    return done ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string command = argc > 1 ? argv[1] : "status";
    std::filesystem::path adir = adapters_dir(argc, argv);

    if (command == "status" || command == "install" || command == "uninstall") {
        auto adapters = select_adapters(argc, argv, adir);
        if (adapters.empty()) {
            std::cerr << "no adapters found in " << adir.string() << "\n";
            return 1;
        }
        for (const auto& a : adapters) {
            if (command == "status")
                print_status(a);
            else if (command == "install")
                return cmd_install(a, argc, argv);
            else
                return cmd_uninstall(a, argc, argv);
        }
        return 0;
    }

    std::cout <<
        "usage: lumen <status|install|uninstall> [--adapter <id>] "
        "[--artifact <path>|--build] [--all] [--adapters <dir>]\n";
    return command == "help" ? 0 : 1;
}
