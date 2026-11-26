#include "util.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace lumen {

std::filesystem::path expand_path(const std::string& token) {
    std::string s = token;

    auto env = [](const std::string& name) -> const char* {
#ifdef _WIN32
        return std::getenv(name.c_str());
#else
        return std::getenv(name.c_str());
#endif
    };

    // %VAR% (Windows style)
    for (size_t at = s.find('%'); at != std::string::npos; at = s.find('%')) {
        size_t end = s.find('%', at + 1);
        if (end == std::string::npos)
            break;
        std::string name = s.substr(at + 1, end - at - 1);
        const char* val = env(name);
        s.replace(at, end - at + 1, val ? val : "");
    }
    // $VAR at segment starts (POSIX style)
    for (size_t at = s.find('$'); at != std::string::npos; at = s.find('$')) {
        size_t end = at + 1;
        while (end < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_'))
            ++end;
        std::string name = s.substr(at + 1, end - at - 1);
        const char* val = env(name);
        s.replace(at, end - at, val ? val : "");
    }
    if (!s.empty() && s.front() == '~') {
        const char* home = nullptr;
#ifdef _WIN32
        home = env("USERPROFILE");
        if (!home)
            home = env("HOME");
#else
        home = env("HOME");
#endif
        s.replace(0, 1, home ? home : "");
    }
    return std::filesystem::path(s);
}

std::filesystem::path lumen_data_dir() {
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (base)
        return std::filesystem::path(base) / "TwilitRealm" / "Lumen";
    return expand_path("~/.local/share/TwilitRealm/Lumen");
#elif defined(__APPLE__)
    return expand_path("~/Library/Application Support/TwilitRealm/Lumen");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg)
        return std::filesystem::path(xdg) / "TwilitRealm" / "Lumen";
    return expand_path("~/.local/share/TwilitRealm/Lumen");
#endif
}

std::string read_file_string(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

bool write_file_string(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << text;
    return static_cast<bool>(out);
}

std::string iso8601_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<std::string> arg_value(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == name && i + 1 < argc)
            return argv[i + 1];
        if (a.starts_with(name + "="))
            return a.substr(name.size() + 1);
    }
    return std::nullopt;
}

bool arg_flag(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == name)
            return true;
    return false;
}

}  // namespace lumen
