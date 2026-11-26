#include "state.h"

#include "json.h"
#include "util.h"

namespace lumen {
namespace {

std::filesystem::path state_path() { return lumen_data_dir() / "state.json"; }

std::vector<InstallRecord> from_json() {
    std::vector<InstallRecord> out;
    bool ok = false;
    std::string text = read_file_string(state_path(), ok);
    if (!ok)
        return out;
    std::string error;
    auto root = json_parse(text, error);
    if (!root || !root->is_object())
        return out;
    auto installs = root->find("installs");
    if (!installs || !installs->is_array())
        return out;
    for (const auto& item : installs->array) {
        if (!item.is_object())
            continue;
        InstallRecord r;
        if (auto v = item.find("adapter"); v && v->is_string()) r.adapter_id = v->string;
        if (auto v = item.find("mod_dir"); v && v->is_string()) r.mod_dir = v->string;
        if (auto v = item.find("bundle"); v && v->is_string()) r.bundle = v->string;
        if (auto v = item.find("source"); v && v->is_string()) r.source = v->string;
        if (auto v = item.find("installedAt"); v && v->is_string()) r.installedAt = v->string;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace

std::vector<InstallRecord> state_load() { return from_json(); }

void state_save(const std::vector<InstallRecord>& records) {
    JsonValue root;
    root.kind = JsonValue::Kind::Object;
    JsonValue installs;
    installs.kind = JsonValue::Kind::Array;
    for (const auto& r : records) {
        JsonValue item;
        item.kind = JsonValue::Kind::Object;
        auto put = [&](const char* key, const std::string& value) {
            JsonValue v;
            v.kind = JsonValue::Kind::String;
            v.string = value;
            item.object[key] = std::move(v);
        };
        put("adapter", r.adapter_id);
        put("mod_dir", r.mod_dir);
        put("bundle", r.bundle);
        put("source", r.source);
        put("installedAt", r.installedAt);
        installs.array.push_back(std::move(item));
    }
    root.object["installs"] = std::move(installs);
    write_file_string(state_path(), json_write(root) + "\n");
}

void state_record_install(const Adapter& a, const Install& in, const std::string& source) {
    auto records = state_load();
    state_remove_install(a, in);
    InstallRecord r;
    r.adapter_id = a.id;
    r.mod_dir = in.mod_dir.string();
    r.bundle = a.bundle;
    r.source = source;
    r.installedAt = iso8601_now();
    records.push_back(std::move(r));
    state_save(records);
}

void state_remove_install(const Adapter& a, const Install& in) {
    auto records = state_load();
    std::filesystem::path want = in.mod_dir;
    std::erase_if(records, [&](const InstallRecord& r) {
        return r.adapter_id == a.id &&
               std::filesystem::path(r.mod_dir).lexically_normal() == want.lexically_normal();
    });
    state_save(records);
}

std::vector<InstallRecord> state_find(const Adapter& a, const Install& in) {
    std::vector<InstallRecord> out;
    for (const auto& r : state_load()) {
        if (r.adapter_id != a.id)
            continue;
        if (std::filesystem::path(r.mod_dir).lexically_normal() ==
            in.mod_dir.lexically_normal())
            out.push_back(r);
    }
    return out;
}

}  // namespace lumen
