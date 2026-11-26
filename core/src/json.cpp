#include "json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lumen {
namespace {

struct Parser {
    std::string_view text;
    size_t pos = 0;
    std::string error;

    void skip_ws() {
        while (pos < text.size() &&
               (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                text[pos] == '\r'))
            ++pos;
    }

    bool fail(std::string msg) {
        if (error.empty())
            error = std::move(msg) + " at offset " + std::to_string(pos);
        return false;
    }

    bool consume(char c) {
        skip_ws();
        if (pos < text.size() && text[pos] == c) {
            ++pos;
            return true;
        }
        fail(std::string("expected '") + c + "'");
        return false;
    }

    bool parse_value(JsonValue& out);

    bool parse_string(std::string& out) {
        if (!consume('"'))
            return false;
        out.clear();
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"')
                return true;
            if (c == '\\') {
                if (pos >= text.size())
                    return fail("unterminated escape");
                char e = text[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos + 4 > text.size())
                            return fail("bad \\u escape");
                        unsigned cp = std::strtoul(
                                std::string(text.substr(pos, 4)).c_str(), nullptr, 16);
                        pos += 4;
                        // UTF-8 encode (BMP only; surrogates passed through as is).
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }
};

bool Parser::parse_value(JsonValue& out) {
    skip_ws();
    if (pos >= text.size())
        return fail("unexpected end of input");
    char c = text[pos];
    if (c == '{') {
        ++pos;
        out.kind = JsonValue::Kind::Object;
        skip_ws();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            JsonValue key;
            std::string name;
            if (!parse_string(name))
                return false;
            if (!consume(':'))
                return false;
            JsonValue value;
            if (!parse_value(value))
                return false;
            out.object.emplace(std::move(name), std::move(value));
            skip_ws();
            if (pos < text.size() && text[pos] == ',') {
                ++pos;
                continue;
            }
            return consume('}');
        }
    }
    if (c == '[') {
        ++pos;
        out.kind = JsonValue::Kind::Array;
        skip_ws();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parse_value(item))
                return false;
            out.array.push_back(std::move(item));
            skip_ws();
            if (pos < text.size() && text[pos] == ',') {
                ++pos;
                continue;
            }
            return consume(']');
        }
    }
    if (c == '"') {
        std::string s;
        if (!parse_string(s))
            return false;
        out.kind = JsonValue::Kind::String;
        out.string = std::move(s);
        return true;
    }
    if (text.substr(pos).starts_with("true")) {
        pos += 4;
        out.kind = JsonValue::Kind::Bool;
        out.boolean = true;
        return true;
    }
    if (text.substr(pos).starts_with("false")) {
        pos += 5;
        out.kind = JsonValue::Kind::Bool;
        out.boolean = false;
        return true;
    }
    if (text.substr(pos).starts_with("null")) {
        pos += 4;
        out.kind = JsonValue::Kind::Null;
        return true;
    }
    // number
    char* end = nullptr;
    std::string num_text(text.substr(pos, 32));
    double d = std::strtod(num_text.c_str(), &end);
    if (!end || end == num_text.c_str())
        return fail("invalid value");
    pos += static_cast<size_t>(end - num_text.c_str());
    out.kind = JsonValue::Kind::Number;
    out.number = d;
    return true;
}

void write_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    out.push_back('"');
}

void write_value(std::string& out, const JsonValue& v) {
    switch (v.kind) {
        case JsonValue::Kind::Null: out += "null"; break;
        case JsonValue::Kind::Bool: out += v.boolean ? "true" : "false"; break;
        case JsonValue::Kind::Number: {
            if (v.number == std::trunc(v.number) && std::fabs(v.number) < 1e15) {
                out += std::to_string(static_cast<long long>(v.number));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", v.number);
                out += buf;
            }
            break;
        }
        case JsonValue::Kind::String: write_string(out, v.string); break;
        case JsonValue::Kind::Array: {
            out.push_back('[');
            for (size_t i = 0; i < v.array.size(); ++i) {
                if (i)
                    out.push_back(',');
                write_value(out, v.array[i]);
            }
            out.push_back(']');
            break;
        }
        case JsonValue::Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, val] : v.object) {
                if (!first)
                    out.push_back(',');
                first = false;
                write_string(out, k);
                out.push_back(':');
                write_value(out, val);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

std::optional<JsonValue> json_parse(std::string_view text, std::string& error) {
    Parser p{text, 0, {}};
    JsonValue root;
    if (!p.parse_value(root)) {
        error = p.error;
        return std::nullopt;
    }
    p.skip_ws();
    if (p.pos != p.text.size()) {
        error = "trailing data at offset " + std::to_string(p.pos);
        return std::nullopt;
    }
    return root;
}

std::string json_write(const JsonValue& value) {
    std::string out;
    write_value(out, value);
    return out;
}

}  // namespace lumen
