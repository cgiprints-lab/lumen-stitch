// Minimal JSON reader/writer for adapter manifests and state files.
// Enough for objects/arrays/strings/numbers/bools; no locale dependence.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumen {

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    JsonObject object;

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_string() const { return kind == Kind::String; }
    bool is_bool() const { return kind == Kind::Bool; }

    const JsonValue* find(std::string_view key) const {
        auto it = object.find(std::string(key));
        return it == object.end() ? nullptr : &it->second;
    }
    const std::vector<JsonValue>* as_array() const {
        return is_array() ? &array : nullptr;
    }
    std::optional<std::string> as_string() const {
        if (is_string()) return string;
        return std::nullopt;
    }
};

// Parses JSON text. Returns nullopt and fills error on failure.
std::optional<JsonValue> json_parse(std::string_view text, std::string& error);

// Serializes a value built with Object/Array kinds. Compact form.
std::string json_write(const JsonValue& value);

}  // namespace lumen
