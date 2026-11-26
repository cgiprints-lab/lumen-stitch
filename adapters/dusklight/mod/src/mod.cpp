// Lumen Photo Mode - Dusklight integration, M0 skeleton.
//
// M0 scope: prove the out-of-tree integration chain - the mod loads, shows an
// in-game panel, toggles a photo session (toast), and persists settings.
// M1 adds time freeze (HookService) + free camera (CameraService operator) +
// 1x HUD-free capture. M2 adds gigapixel tiling via GfxService/WebGPU.
#include "mods/api.h"

#if defined(__MINGW32__)
// Compile-check-only shim for local MinGW builds. Two incompatibilities, both
// harmless on the supported toolchains (MSVC/clang-cl, where this is skipped):
//  1. DEFINE_MOD expands to `MOD_EXPORT constinit const ModMeta mod_meta`; C++
//     `const` has internal linkage and GCC rejects dllexport on it (MSVC does
//     not). `extern` restores external linkage.
//  2. The modmeta section markers (`#pragma section` / __declspec(allocate))
//     are ignored by MinGW, so a MinGW-built bundle would NOT load in
//     Dusklight even if it links. CI builds (MSVC matrix) are authoritative.
// Keep this after api.h (which defines MOD_EXPORT) and before service.hpp.
#undef MOD_EXPORT
#define MOD_EXPORT __declspec(dllexport) extern
#endif

#include "mods/service.hpp"

#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

namespace {

struct SessionState {
    bool active = false;
};
SessionState g_session;

// Settings surfaced in the mod panel; consumed by the M1/M2 capture pipeline.
ConfigVarHandle g_var_capture_scale = 0;  // gigapixel multiplier (2..33)
ConfigVarHandle g_var_settle_frames = 0;  // frames to settle before each shot

ConfigVarHandle register_int_var(const char* name, int64_t def) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    ConfigVarHandle var = 0;
    if (svc_config->register_var(mod_ctx, &desc, &var) != MOD_OK)
        mods::log::warn("failed to register config var {}", name);
    return var;
}

UiElementHandle g_status_text = 0;

const char* status_label() {
    return g_session.active ? "photo session: ACTIVE (capture pipeline arrives in M1/M2)"
                            : "photo session: inactive";
}

ModResult push_session_toast(const char* title, const char* body) {
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = "success";
    toast.title_rml = title;
    toast.body_rml = body;
    toast.duration_ms = 3000;
    return svc_ui->push_toast(mod_ctx, &toast);
}

void on_start_pressed(ModContext*, void*) {
    if (g_session.active)
        return;
    g_session.active = true;
    mods::log::info("photo session started (M0 skeleton)");
    push_session_toast("Lumen Photo Mode", "Photo session started.");
}

void on_stop_pressed(ModContext*, void*) {
    if (!g_session.active)
        return;
    g_session.active = false;
    mods::log::info("photo session stopped");
    push_session_toast("Lumen Photo Mode", "Photo session ended.");
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Photo session");
    svc_ui->pane_add_text(mod_ctx, panel, status_label(), &g_status_text);

    UiControlDesc start = UI_CONTROL_DESC_INIT;
    start.kind = UI_CONTROL_BUTTON;
    start.label = "Start photo session";
    start.help_rml = "Freezes the scene and takes over the camera. "
                     "Free camera controls arrive in M1.";
    start.on_pressed = on_start_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &start, nullptr);

    UiControlDesc stop = UI_CONTROL_DESC_INIT;
    stop.kind = UI_CONTROL_BUTTON;
    stop.label = "End photo session";
    stop.on_pressed = on_stop_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &stop, nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Capture settings");

    UiControlDesc scale = UI_CONTROL_DESC_INIT;
    scale.kind = UI_CONTROL_NUMBER;
    scale.label = "Super resolution scale";
    scale.help_rml = "Gigapixel capture multiplier (2-33). Used by the M2 pipeline.";
    scale.binding = UI_BINDING_CONFIG_VAR;
    scale.config_var = g_var_capture_scale;
    scale.min = 2;
    scale.max = 33;
    scale.suffix = "x";
    svc_ui->pane_add_control(mod_ctx, panel, &scale, nullptr);

    UiControlDesc settle = UI_CONTROL_DESC_INIT;
    settle.kind = UI_CONTROL_NUMBER;
    settle.label = "Settle frames per tile";
    settle.help_rml = "Frames to present before snapshotting a tile so temporal "
                      "effects converge.";
    settle.binding = UI_BINDING_CONFIG_VAR;
    settle.config_var = g_var_settle_frames;
    settle.min = 0;
    settle.max = 10;
    svc_ui->pane_add_control(mod_ctx, panel, &settle, nullptr);

    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_status_text)
        svc_ui->elem_set_text(mod_ctx, g_status_text, status_label());
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_var_capture_scale = register_int_var("captureScale", 4);
    g_var_settle_frames = register_int_var("settleFrames", 2);

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        mods::set_error(error, MOD_ERROR, "failed to register mods panel");
        return MOD_ERROR;
    }

    mods::log::info("Lumen Photo Mode {} loaded (M0 skeleton)", "0.1.0");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    // M1: hotkey handling, freeze hooks and camera operator active here.
    (void)error;
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    g_session.active = false;
    (void)error;
    return MOD_OK;
}

}  // extern "C"
