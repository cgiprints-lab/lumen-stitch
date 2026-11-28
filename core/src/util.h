// Small path/env helpers shared by the CLI.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lumen {

// Expands a manifest path token: leading '~', '$VAR' and '%VAR%' forms.
std::filesystem::path expand_path(const std::string& token);

// Lumen's own data directory (state files live here).
std::filesystem::path lumen_data_dir();

std::string read_file_string(const std::filesystem::path& path, bool& ok);
bool write_file_string(const std::filesystem::path& path, const std::string& text);

// Recursively copies a folder (a folder-shaped integration bundle, as games
// whose mod format is a folder with a manifest expect). Same copy semantics
// as copy_file's overwrite_existing, over an existing tree.
void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to,
               std::error_code& ec);

std::string iso8601_now();

// Append (argv-style) helpers for main().
std::optional<std::string> arg_value(int argc, char** argv, const std::string& name);
bool arg_flag(int argc, char** argv, const std::string& name);

}  // namespace lumen
