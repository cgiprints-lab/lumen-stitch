// Lumen's install ledger: <lumen_data_dir>/state.json
#pragma once

#include "adapter.h"
#include "detect.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lumen {

struct InstallRecord {
    std::string adapter_id;
    std::string mod_dir;
    std::string bundle;
    std::string source;      // "artifact" | "build" | path origin
    std::string installedAt;
};

std::vector<InstallRecord> state_load();
void state_save(const std::vector<InstallRecord>& records);

// Upsert/remove records around install/uninstall operations.
void state_record_install(const Adapter& a, const Install& in, const std::string& source);
void state_remove_install(const Adapter& a, const Install& in);
std::vector<InstallRecord> state_find(const Adapter& a, const Install& in);

}  // namespace lumen
