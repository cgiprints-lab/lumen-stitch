// Lumen Photo Mode - Dusklight integration (Ansel-style photography for Dusklight).
//
// M3 scope: everything from M2 (photo sessions with a free camera via a
// CameraService operator, world freeze via the game's own global actor-pause
// byte, HUD-free native-resolution capture via GfxService
// resolve_pass at FRAME_BEFORE_HUD + WebGPU compute readback, hotkeys, capture
// folder persistence, and gigapixel capture: an S x S grid of sub-frustum
// re-projections sharing one eye - Ansel's gigapixel model - each tile written
// as a PNG plus a JSON manifest for `lumen stitch`), plus the consistency
// layer for multi-part shots: while a gigapixel job runs, screen-space and
// temporal effects that would differ between tiles are suppressed (Ansel's
// "disable non-uniform and temporal effects for multipart captures" rule):
//   - moya cloud-shadow haze: pre-hook on drawCloudShadow, skipping the draw
//     per frame (same pattern as the upstream effect_remover mod), and
//   - bloom / fade / motion blur: state lives in mDoGph_gInf_c statics, so the
//     values themselves are pinned to off through HookService::resolve data
//     symbols and restored when the job ends.
// Every symbol is resolved from the shipped manifest and each override degrades
// gracefully (log + skip) when a name is missing or ambiguous.
//
// Controls (Windows; panel buttons cover every action on other platforms):
//   F1 photo mode   F7 capture 1x   F12 gigapixel   Shift+F12 360 pano
//   Shift+F7 stereo trio
//   WASD move, Q/E down/up, arrows look, Shift boost, PgUp/PgDn field of view.
// (F8-F11 are the game's own hotkeys - state share, debug camera, audio
// debug, fullscreen toggle - and must not be claimed.)
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

#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/file.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include <webgpu/webgpu.h>

#include <cinttypes>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/tilemath.h"
#include "stitcher/src/stitch.h"

namespace tilemath = lumen::tilemath;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Declarations only: stitch.cpp (compiled into this module) owns the stb
// implementations (both image read and write) - defining them here too would
// duplicate every stb_image_write symbol at link time.
#include "stb_image_write.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(FileService, svc_file);
IMPORT_SERVICE(HookService, svc_hook);

// Hook targets, resolved from the game symbol manifest at load. Absence degrades
// gracefully: no auto-exit rather than a dead mod.
//
// RETIRED (2026-11-26): a dusk::game_clock::advance() post-hook used to zero
// FrameTiming's numSimTicks/dt as the photo-mode freeze. It was inert on the
// default presentation path (that path ignores the tick count) and writing
// dt=0 into the shared clock wedged the game's execute loop - after that no
// further mod_update ran, so hotkeys and capture died while the pause byte
// kept the world standing still. The freeze is now purely the game's own
// global actor pause (see world_freeze_begin), exactly what the `freeze`
// console command sets.
DEFINE_HOOK_SYMBOL("dStage_changeScene", int(int, float, unsigned, signed char, short, int),
    StageChangeScene);
// Moya cloud-shadow haze draw entry (d_kankyo_rain.h: drawCloudShadow(Mtx, u8**)).
// Mtx is a float[3][4] typedef; it decays to a pointer in the call ABI, so the
// free-style signature below matches the game's trampoline requirements.
DEFINE_HOOK_SYMBOL("drawCloudShadow", void(float*, unsigned char**), CloudShadow);

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// ---------------------------------------------------------------- state

enum CapturePhase {
    CAPTURE_NONE = 0,
    CAPTURE_WAIT_SETTLE, // countdown running before the snapshot
    CAPTURE_WAIT_COPY,   // compute recorded this frame; map after submission
    CAPTURE_WAIT_MAP,    // staging buffer mapping in flight
};

struct PhotoState {
    bool active = false;
    bool frozen = false; // time freeze engaged this session
    bool seeded = false; // camera operator adopted the game camera
    float eye[3] = {0, 0, 0};
    float yaw = 0;   // radians
    float pitch = 0; // radians
    float fovy = 90; // degrees
    float fov_override = 0; // per-tile FOV during a gigapixel job (0 = none)
    float eye_off[3] = {0, 0, 0}; // per-tile eye shift (stereo); 0 for grid/360
    // The operator mutates the game's live camera state, so the pose it had
    // before we took over is captured at seed time and handed back for a few
    // frames on exit - otherwise the game resumes from the photo pose and
    // can stay parked at whatever zoom/angle the shot left behind.
    bool have_saved = false;
    int restore_frames = 0;
    float saved_eye[3] = {0, 0, 0};
    float saved_center[3] = {0, 0, 0};
    float saved_fovy = 0;
    float saved_bank = 0;
};
PhotoState g_photo;

struct CaptureState {
    CapturePhase phase = CAPTURE_NONE;
    int settle_left = 0;
    int stall_frames = 0;        // watchdog: frames since begin_capture
    uint32_t width = 0;
    uint32_t height = 0;
    bool for_job = false;          // this capture is a gigapixel tile
    std::vector<char> name;        // file name override (utf-8), else timestamped
    // Mod-owned per-capture readback objects.
    WGPUBuffer staging = nullptr;  // MAP_READ | COPY_DST
    WGPUBuffer storage = nullptr;  // STORAGE | COPY_SRC (compute output)
    uint32_t uniform_offset = 0;   // dims slot inside the frame uniform stream
    // Persistent pipeline.
    WGPUComputePipeline pipeline = nullptr;
    uint32_t buf_bytes = 0;        // size of the live staging/storage pair (0 = none)
    bool map_failed = false;       // set by the map callback on non-Success status
};
CaptureState g_capture;

// Completion callback for wgpuBufferMapAsync. Both halves of the contract are
// mandatory in this Dawn build: callbackMode must be non-zero (the INIT macro
// deliberately zero-fills it - passing it yields "Invalid callback mode: 0"),
// and Dawn completes *error* futures by invoking the callback, so a null one
// is a call through 0x0 (the crash in the v2.0.3 acceptance log). Callback
// mode AllowProcessEvents fires on the thread that ticks the device - the
// game thread, which is also the one running mod_update.
void on_map_complete(WGPUMapAsyncStatus status, WGPUStringView /*message*/,
    void* /*userdata1*/, void* /*userdata2*/) {
    if (status != WGPUMapAsyncStatus_Success)
        g_capture.map_failed = true;  // consumed by mod_update -> fail_capture
}

// --- gigapixel job ----------------------------------------------------------
// One job = an S x S grid of tiles re-projecting sub-frusta of the master
// camera (same eye). Tiles aim through the existing camera operator; each
// tile is one capture at native size. The manifest written at the end carries
// per-tile pose so the external stitcher can resample exactly.
struct JobTileDef {
    int row = 0, col = 0;
    double yaw = 0, pitch = 0; // radians (pose applied to the operator)
    double fovy = 90;          // degrees
    double ox = 0, oy = 0, oz = 0; // eye offset in world units (stereo only)
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // master-pixel rect owned
    std::vector<char> name;    // utf-8 tile PNG name
};
struct JobState {
    enum Kind { GRID, CUBE360, STEREO } kind = GRID;
    bool active = false;
    bool build_pending = false; // waiting on the viewport-size probe
    int probe_frames = 0;       // probe watchdog (frames while build_pending)
    uint32_t probe_w = 0, probe_h = 0; // last probe reading (must repeat once)
    int rebuilds = 0;           // mid-job grid rebuilds (transient resizes)
    uint32_t S = 0;             // tiles per side, frozen at job start (GRID)
    uint32_t vw = 0, vh = 0;    // scene color size (probe)
    WGPUTextureFormat color_format = WGPUTextureFormat_Undefined; // probe (see manifest)
    uint64_t id = 0;            // timestamp shared by tile names + manifest
    std::vector<JobTileDef> tiles;
    size_t idx = 0;
    std::string json; // manifest text, tiles appended as each one completes
};
JobState g_job;

// --- gigapixel consistency layer (M3) ---------------------------------------
// Multi-part shots re-render the same frozen scene from different sub-frusta,
// so any effect whose output depends on the *viewport* rather than the world
// (screen-space haze, bloom, fade) or on *time flow* (motion blur) disagrees
// between tiles. World-space effects stay consistent under the time freeze and
// are left alone. While a job runs:
//   haze   - drawCloudShadow skipped per frame (pre-hook, HOOK_SKIP_ORIGINAL),
//   bloom  - mDoGph_gInf_c::m_bloom.mEnable (offset 0x08 in bloom_c) pinned off,
//   fade   - mDoGph_gInf_c::mFade (u8) pinned to 0,
//   blur   - mDoGph_gInf_c::mBlureFlag (u8) pinned off.
// The data symbols are resolved once at load (the manifest carries non-exported
// statics too); each one degrades to "not available" on MOD_UNAVAILABLE or
// MOD_CONFLICT rather than failing the mod. Static *data* is keyed by its
// mangled name in the manifest (symgen pulls PDB data records verbatim), so
// each pin tries the mangled spelling first, then the qualified one in case
// a DISPLAY alias exists. Sizes must match the declared type exactly - mFade
// and mBlureFlag are u8, pinning 4 bytes would clobber the neighbour.
struct DataPin {
    const char* symbol = nullptr;      // first-choice manifest name
    const char* alt_symbol = nullptr;  // fallback spelling (null = none)
    size_t offset = 0;                 // field offset inside the resolved object
    size_t size = 0;                   // bytes pinned (exact type size)
    ConfigVarHandle toggle = 0;        // user opt-in/opt-out var
    bool default_on = true;            // effective value when the var is unset
    void* addr = nullptr;              // resolved address (null = unavailable)
    unsigned char saved[8] = {0};      // original bytes, snapshotted at job start
    bool pinned = false;
};
DataPin g_pins[] = {
    // MSVC static *class member* data: storage char '2'; bool -> _N, u8 -> E,
    // class object -> PEAV<type>@@; decoration terminates in a single 'A'.
    // mBlureFlag is declared u8, not bool: storage suffix E, not _N.
    {"?mBlureFlag@mDoGph_gInf_c@@2EA", "mDoGph_gInf_c::mBlureFlag", 0, 1, 0, true, nullptr, {},
        false},
    // Bloom, fade and the cloud-shadow haze stay ON by default: they are the
    // additive/overlay passes the player actually sees, and removing them makes
    // captures visibly darker and flatter than the game. Motion blur is the one
    // that must go - it is an EFB trail, so tile N would ghost tile N-1.
    {"?mFade@mDoGph_gInf_c@@2EA", "mDoGph_gInf_c::mFade", 0, 1, 0, false, nullptr, {}, false},
    // bloom_c is nested inside mDoGph_gInf_c, and m_bloom is an object, not a
    // pointer. MSVC compresses the nested *type* reference with a back-token:
    // the shipped manifest key is ?m_bloom@mDoGph_gInf_c@@2Vbloom_c@1@A
    // (extracted verbatim from the embedded manifest in dusklight.exe).
    {"?m_bloom@mDoGph_gInf_c@@2Vbloom_c@1@A", "mDoGph_gInf_c::m_bloom", 0x08, 1, 0, false, nullptr,
        {}, false},
};

// Photo mode's "the world stands still" switch. Zeroing FrameTiming only slows
// the fixed-step sim down: with the default settings (frame interpolation off)
// the game runs its whole logic pass every render frame regardless of the tick
// count, which is why characters, NPCs and particles kept moving. What actually
// stops them is the game's own global freeze - the "freeze" console command
// sets exactly this byte (src/dusk/commands.cpp), and fopAc_Execute skips every
// actor's per-frame execute while it is set (src/f_op/f_op_actor.cpp:323).
// mPause is at 0x4 in dComIfAc_info_c (include/d/d_com_inf_actor.h); the
// manifest key was read out of dusklight.exe (storage char '3' = static object).
DataPin g_world_pin = {"?g_dComIfAc_gameInfo@@3VdComIfAc_info_c@@A", "g_dComIfAc_gameInfo", 0x04, 1,
    0, true, nullptr, {}, false};

bool g_haze_hooked = false; // drawCloudShadow pre-hook installed

// Latest world camera, refreshed at GFX_STAGE_SCENE_BEGIN (world-camera stage).
CameraInfo g_camera = CAMERA_INFO_INIT;
bool g_camera_valid = false;

// Stage hook handles (the service requires a non-null out-param; the game
// validates it - passing nullptr fails with MOD_INVALID_ARGUMENT).
GfxStageHookHandle g_hook_scene_begin = 0;
GfxStageHookHandle g_hook_frame = 0;

// ------------------------------------------------------------ feature fallback
//
// Version-fallback matrix at runtime: every subsystem that can be missing on
// an older Dusklight (fewer service minors, different symbol manifest)
// degrades independently at load (see mod_initialize). The floor is the 1x
// capture path - stage hooks + readback compute - and without it photo mode
// itself stays off with an explanation rather than half-working. The exact
// set is logged once so support reports can see what a build actually got.
bool g_feat_capture = false; // stage hooks + compute type -> 1x + jobs
bool g_feat_world_freeze = false; // game's own global actor-pause byte
bool g_feat_autoexit = false; // dStage_changeScene hook
bool g_feat_strip = false;   // haze hook or >=1 data pin resolved

// Config vars.
ConfigVarHandle g_var_capture_scale = 0;
ConfigVarHandle g_var_settle_frames = 0;
ConfigVarHandle g_var_move_speed = 0;
ConfigVarHandle g_var_look_speed = 0;
ConfigVarHandle g_var_stereo_ipd = 0;
ConfigVarHandle g_var_photo_fov = 0;
ConfigVarHandle g_var_capture_folder = 0;
ConfigVarHandle g_var_strip_haze = 0;
ConfigVarHandle g_var_essentials_hint = 0;
bool g_compat_hinted = false;
ConfigVarHandle g_var_auto_stitch = 0;
ConfigVarHandle g_var_freeze_world = 0;
ConfigVarHandle g_var_swap_rb = 0;
ConfigVarHandle g_var_seen_controls = 0;

// Background-stitch state (worker thread <-> game thread). See the section
// near folder handling for the full contract.
std::thread g_stitch_worker;
std::atomic<bool> g_stitch_running{false};
std::atomic<uint32_t> g_stitch_percent{0};
std::mutex g_stitch_mtx;
std::string g_stitch_done;   // non-empty = finished, awaiting toast on game thread
bool g_stitch_done_ok = false;
ConfigVarHandle g_var_strip_bloom = 0;
ConfigVarHandle g_var_strip_fade = 0;
ConfigVarHandle g_var_strip_blur = 0;

char g_folder_location[1024] = {0}; // persisted opaque FileService location

// ---------------------------------------------------------------- config helpers

ConfigVarHandle register_var_typed(const char* name, ConfigVarType type, int64_t i, double f) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
    desc.default_bool = type == CONFIG_VAR_BOOL && i != 0;
    desc.default_int = i;
    desc.default_float = f;
    ConfigVarHandle var = 0;
    if (svc_config->register_var(mod_ctx, &desc, &var) != MOD_OK)
        mods::log::warn("lumen: failed to register config var {}", name);
    return var;
}

int64_t cfg_int(ConfigVarHandle var, int64_t def) {
    int64_t v = def;
    if (var && svc_config->get_int(mod_ctx, var, &v) != MOD_OK)
        v = def;
    return v;
}

bool cfg_bool(ConfigVarHandle var, bool def) {
    bool v = def;
    if (var && svc_config->get_bool(mod_ctx, var, &v) != MOD_OK)
        v = def;
    return v;
}

void cfg_string(ConfigVarHandle var, char* buffer, size_t size) {
    if (buffer && size)
        buffer[0] = '\0';
    if (!var || !buffer || !size)
        return;
    svc_config->get_string(mod_ctx, var, buffer, size, nullptr);
}

// ------------------------------------------- effect pins (gigapixel consistency)

// Resolve the mDoGph_gInf_c post-effect state symbols and the actor-freeze flag
// once at load. The names come from the shipped symbol manifest; missing or
// ambiguous names just mean the matching override is unavailable (logged, and
// the panel toggle stays inert).
bool resolve_pin(DataPin& pin) {
    const char* names[2] = {pin.symbol, pin.alt_symbol};
    for (const char* name : names) {
        if (!name)
            continue;
        void* addr = nullptr;
        HookSymbolFlags flags = {};
        const ModResult r = svc_hook->resolve(mod_ctx, name, &addr, &flags);
        if (r == MOD_OK && addr) {
            pin.addr = addr;
            mods::log::info("lumen: pinned effect state '{}' flags={:04x}", name,
                static_cast<unsigned>(flags));
            return true;
        }
    }
    mods::log::warn("lumen: effect symbol '{}' unavailable; override off", pin.symbol);
    return false;
}

void resolve_effect_symbols() {
    for (DataPin& pin : g_pins)
        resolve_pin(pin);
    g_feat_world_freeze = resolve_pin(g_world_pin);
}

// Snapshot originals then force off, honouring the user toggles.
void strip_begin() {
    for (DataPin& pin : g_pins) {
        pin.pinned = false;
        if (!pin.addr || !cfg_bool(pin.toggle, pin.default_on))
            continue;
        std::memcpy(pin.saved, static_cast<unsigned char*>(pin.addr) + pin.offset, pin.size);
        std::memset(static_cast<unsigned char*>(pin.addr) + pin.offset, 0, pin.size);
        pin.pinned = true;
    }
}

// Re-assert the zeros in case the game touched them mid-job (belt and braces;
// the time freeze normally keeps this state still).
void strip_repin() {
    for (DataPin& pin : g_pins) {
        if (pin.pinned)
            std::memset(static_cast<unsigned char*>(pin.addr) + pin.offset, 0, pin.size);
    }
}

void strip_end() {
    for (DataPin& pin : g_pins) {
        if (!pin.pinned)
            continue;
        std::memcpy(static_cast<unsigned char*>(pin.addr) + pin.offset, pin.saved, pin.size);
        pin.pinned = false;
    }
}

// Photo-mode world freeze: set the game's global actor-pause byte, which is
// what the game's own "freeze" console command does. Entered on photo-mode
// entry, released on exit (and on unload), with the previous value restored.
std::chrono::steady_clock::time_point g_photo_remind_at{};

void world_freeze_begin() {
    if (!g_world_pin.addr || g_world_pin.pinned || !cfg_bool(g_var_freeze_world, true))
        return;
    unsigned char* p = static_cast<unsigned char*>(g_world_pin.addr) + g_world_pin.offset;
    std::memcpy(g_world_pin.saved, p, g_world_pin.size);
    *p = 1;
    g_world_pin.pinned = true;
}

void world_freeze_end() {
    if (!g_world_pin.pinned)
        return;
    unsigned char* p = static_cast<unsigned char*>(g_world_pin.addr) + g_world_pin.offset;
    // Only undo our own pause: if the game itself changed the byte while we
    // held it (its own freeze/menu toggles), leave its value alone.
    if (*p == 1)
        *p = g_world_pin.saved[0];
    g_world_pin.pinned = false;
}

// drawCloudShadow pre-hook: skip the moya haze draw while a job is running.
HookAction on_cloud_shadow_pre(ModContext*, void*, void*, void*) {
    if (g_job.active && g_haze_hooked && cfg_bool(g_var_strip_haze, false))
        return HOOK_SKIP_ORIGINAL;
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------- ui helpers

UiElementHandle g_status_text = 0;
UiElementHandle g_toggle_button = 0;

const char* status_label() {
    static char buf[128];
    if (!g_feat_capture)
        return "capture unavailable in this build (see log)";
    if (g_job.active) {
        if (g_job.build_pending)
            return "capture: measuring viewport...";
        const char* what = g_job.kind == JobState::CUBE360   ? "360: face %zu/%zu"
                           : g_job.kind == JobState::STEREO   ? "stereo: view %zu/%zu"
                                                              : "gigapixel: tile %zu/%zu";
        std::snprintf(buf, sizeof(buf), what, g_job.idx + 1, g_job.tiles.size());
        return buf;
    }
    if (!g_photo.active)
        return "photo mode: off (F1)";
    if (g_stitch_running) {
        static char sbuf[64];
        std::snprintf(sbuf, sizeof(sbuf), "background stitch: %" PRIu32 "%%",
            g_stitch_percent.load());
        return sbuf;
    }
    return g_photo.frozen || g_world_pin.pinned
               ? "PHOTO MODE ON - the world is stopped. Press F1 to play again."
               : "PHOTO MODE ON - press F1 to play again.";
}

void push_photo_toast(const char* title, const char* body, const char* type = "success",
    uint32_t duration_ms = 3000) {
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = type;
    toast.title_rml = title;
    toast.body_rml = body;
    toast.duration_ms = duration_ms;
    svc_ui->push_toast(mod_ctx, &toast);
}

// ---------------------------------------------------------------- help / controls
//
// Photo mode is easy to get lost in: the camera leaves the player, the world
// stops, and nothing on screen says how to get back. So the control map is
// stated three ways - a long toast on entry, a first-run dialog, and a
// permanent block at the top of the mods panel.

constexpr char kControlsRml[] =
    "<p><b>F1</b> photo mode on / off - <b>F1 is the way back to the game</b>.</p>"
    "<p><b>F7</b> capture 1x &nbsp; <b>Shift+F7</b> stereo trio<br>"
    "<b>F12</b> gigapixel &nbsp; <b>Shift+F12</b> 360 panorama<br>"
    "If an overlay (Steam screenshots use plain F12) steals those keys, the "
    "<b>Alt+F12</b> and <b>Alt+Shift+F12</b> variants work instead.</p>"
    "<p><b>WASD</b> fly, <b>Q</b>/<b>E</b> down/up, <b>arrow keys</b> look, "
    "<b>Shift</b> boost, <b>PgUp</b>/<b>PgDn</b> zoom.</p>"
    "<p>Gigapixel, 360 and stereo jobs stitch themselves into a BigTIFF in the "
    "background - keep playing while it runs. The mod never touches the game's "
    "window or display settings: the game's own <b>F11</b> toggles fullscreen "
    "(it sits right next to our F12), and no Lumen key can change it.</p>";

constexpr const char* kPanelControlLines[] = {
    "F1 photo mode on/off - F1 also returns you to the game",
    "F7 capture 1x - Shift+F7 stereo - F12 or Alt+F12 gigapixel - Shift+F12 360",
    "WASD fly, Q/E down/up, arrow keys look, Shift boost, PgUp/PgDn zoom",
};

void on_controls_dismiss(ModContext*, UiDialogHandle, void*) {}

void push_controls_dialog() {
    UiDialogAction actions[] = {
        {sizeof(UiDialogAction), "Got it", on_controls_dismiss, nullptr, false, nullptr},
    };
    UiDialogDesc desc = UI_DIALOG_DESC_INIT;
    desc.title = "Lumen Photo Mode - controls";
    desc.body_rml = kControlsRml;
    desc.actions = actions;
    desc.action_count = sizeof(actions) / sizeof(actions[0]);
    static UiDialogHandle handle = 0;
    if (svc_ui->dialog_push(mod_ctx, &desc, &handle) != MOD_OK)
        push_photo_toast("Lumen Photo Mode", "F1 photo mode - F7 capture - F12 gigapixel", "info",
            8000);
}

void on_show_controls_pressed(ModContext*, void*) {
    push_controls_dialog();
}

// ---------------------------------------------------------------- camera operator

// Which graphics-service call we are inside when something throws: the
// loader's own Dawn wrapper is the prime suspect and this names it.
static const char* g_gfx_stage = "idle";

// The game calls stage hooks and the camera operator WITHOUT a try/catch
// (unlike mod_update, which the loader guards): any C++ exception escaping
// here lands in std::terminate and takes the game down. Shield everything.
struct CallbackShield {
    const char* what;
    bool fired = false;
    std::string message;
    explicit CallbackShield(const char* w) : what(w) {}
    ~CallbackShield() {
        if (fired)
            mods::log::error("lumen: exception swallowed in {}: {} [gfx stage: {}]", what,
                message, g_gfx_stage);
    }
};

// Thunks the game runs raw (hooks/operators) must never let a throw escape.
#define LUMEN_SHIELD_TRY try {
#define LUMEN_SHIELD_CATCH(shield)                                                       \
    }                                                                                    \
    catch (const std::exception& e) {                                                    \
        (shield).fired = true;                                                           \
        (shield).message = e.what();                                                     \
    }                                                                                    \
    catch (...) {                                                                        \
        (shield).fired = true;                                                           \
        (shield).message = "non-standard";                                               \
    }

bool photo_operate(ModContext*, CameraOperatorState* state, void*) {
    CallbackShield shield {"camera operator"};
    LUMEN_SHIELD_TRY
    if (!g_photo.active) {
        // Hand the pristine pose back for a few frames so the game's camera
        // systems resume from where they were, not from the photo pose.
        if (g_photo.have_saved && g_photo.restore_frames > 0) {
            for (int i = 0; i < 3; ++i) {
                state->eye[i] = g_photo.saved_eye[i];
                state->center[i] = g_photo.saved_center[i];
            }
            if (g_photo.saved_fovy > 1.0f)
                state->fovy = g_photo.saved_fovy;
            state->bank_degrees = g_photo.saved_bank;
            if (--g_photo.restore_frames <= 0)
                g_photo.have_saved = false;
            return true;
        }
        return false;
    }
    if (!g_photo.seeded) {
        // First frame under our control: adopt the game camera as the starting pose.
        g_photo.eye[0] = state->eye[0];
        g_photo.eye[1] = state->eye[1];
        g_photo.eye[2] = state->eye[2];
        const float dx = state->center[0] - state->eye[0];
        const float dy = state->center[1] - state->eye[1];
        const float dz = state->center[2] - state->eye[2];
        const float horiz = std::sqrtf(dx * dx + dz * dz);
        g_photo.yaw = std::atan2f(dx, dz);
        g_photo.pitch = std::atan2f(dy, horiz);
        if (state->fovy > 1.0f)
            g_photo.fovy = state->fovy;
        g_photo.seeded = true;
        // Snapshot the game's pose exactly as it was before we touched it.
        for (int i = 0; i < 3; ++i) {
            g_photo.saved_eye[i] = state->eye[i];
            g_photo.saved_center[i] = state->center[i];
        }
        g_photo.saved_fovy = state->fovy;
        g_photo.saved_bank = state->bank_degrees;
        g_photo.have_saved = true;
    }
    const float ex = g_photo.eye[0] + g_photo.eye_off[0];
    const float ey = g_photo.eye[1] + g_photo.eye_off[1];
    const float ez = g_photo.eye[2] + g_photo.eye_off[2];
    state->eye[0] = ex;
    state->eye[1] = ey;
    state->eye[2] = ez;
    const float cp = std::cosf(g_photo.pitch);
    state->center[0] = ex + cp * std::sinf(g_photo.yaw);
    state->center[1] = ey + std::sinf(g_photo.pitch);
    state->center[2] = ez + cp * std::cosf(g_photo.yaw);
    state->fovy = g_photo.fov_override > 0.0f ? g_photo.fov_override : g_photo.fovy;
    state->bank_degrees = 0;
    return true;
    LUMEN_SHIELD_CATCH(shield)
    return false;
}

CameraOperatorHandle g_operator = 0;

// ---------------------------------------------------------------- capture helpers

void reset_capture_buffers() {
    if (g_capture.staging) {
        if (wgpuBufferGetMapState(g_capture.staging) == WGPUBufferMapState_Mapped)
            wgpuBufferUnmap(g_capture.staging);
        wgpuBufferRelease(g_capture.staging);
        g_capture.staging = nullptr;
    }
    if (g_capture.storage) {
        wgpuBufferRelease(g_capture.storage);
        g_capture.storage = nullptr;
    }
    g_capture.buf_bytes = 0;
}

// The staging/storage pair is reused across captures while the scene size
// is unchanged. Creating and freeing a ~25 MB pair per tile exhausted the
// driver's heap during a gigapixel run: the v2.0.3 acceptance log crashed
// inside amdxc64.dll (D3D12 buffer op) at tile 25 with ~640 MB of churn.
bool ensure_capture_buffers(WGPUDevice device, uint32_t bytes) {
    if (g_capture.staging && g_capture.storage && g_capture.buf_bytes == bytes)
        return true;
    reset_capture_buffers();
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.label = {"lumen_staging", WGPU_STRLEN};
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = bytes;
    g_capture.staging = wgpuDeviceCreateBuffer(device, &bd);
    bd.label = {"lumen_storage", WGPU_STRLEN};
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    g_capture.storage = wgpuDeviceCreateBuffer(device, &bd);
    if (!g_capture.staging || !g_capture.storage) {
        reset_capture_buffers();
        return false;
    }
    g_capture.buf_bytes = bytes;
    return true;
}

// ---------------------------------------------------------------- photo session

bool folder_ok() {
    return g_folder_location[0] != '\0' &&
           svc_file->check(mod_ctx, g_folder_location) == MOD_OK;
}

// ---------------------------------------------------------------- background stitch
//
// A completed job is stitched inside the mod, on a worker thread, so the
// player drops straight back into the game instead of running the CLI by
// hand. The stitcher is pure file IO (std::filesystem + stb + BigTIFF writer)
// - it touches neither WebGPU nor the mod services - so running it off the
// game thread is safe; only completion is marshalled back through atomics and
// consumed (toasts) on the game thread in mod_update.
//
// FileService locations are deliberately opaque, but on desktop they are
// plain paths or file:// URIs; anything exotic (Android grants etc.) falls
// back to the manual CLI hint.
bool location_to_path(const char* loc, std::filesystem::path& out) {
    if (!loc || !*loc)
        return false;
    std::string s = loc;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        if (s.compare(0, scheme, "file") != 0)
            return false;
        s = s.substr(scheme + 3);
        std::string dec;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                auto hexv = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    dec += static_cast<char>(hi * 16 + lo);
                    i += 2;
                    continue;
                }
            }
            dec += s[i];
        }
        s = dec;
        // file:///C:/dir -> /C:/dir: drop the slash before a drive letter.
        if (s.size() > 2 && s[0] == '/' && s[2] == ':')
            s.erase(s.begin());
    }
    out = std::filesystem::path(s);
    std::error_code ec;
    return std::filesystem::is_directory(out, ec);
}

void start_auto_stitch(uint64_t job_id) {
    if (!cfg_bool(g_var_auto_stitch, true))
        return;
    if (g_stitch_running) {
        mods::log::info("lumen: auto-stitch skipped for job {}: a stitch is already running",
            job_id);
        return;
    }
    std::filesystem::path dir;
    if (!location_to_path(g_folder_location, dir)) {
        char body[224];
        std::snprintf(body, sizeof(body),
            "Job saved. The capture folder isn't a plain path, so stitching stays manual:\n"
            "lumen stitch lumen-%" PRIu64 ".json", job_id);
        push_photo_toast("Lumen Photo Mode", body, "info");
        return;
    }
    const auto manifest = dir / ("lumen-" + std::to_string(job_id) + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(manifest, ec)) {
        mods::log::warn("lumen: auto-stitch cannot find {}", manifest.string());
        return;
    }
    g_stitch_running = true;
    g_stitch_percent = 0;
    g_stitch_worker = std::thread([manifest, job_id] {
        // Nothing that can throw may escape this thread, and the FAILURE
        // paths must not allocate either: under commit exhaustion (measured
        // 2026-11-26 16:46 and 16:58 - a background app held 48 GB of
        // commit) the stitch's own allocations throw bad_alloc, and a catch
        // block that builds std::strings throws AGAIN while handling it -
        // a throw from a catch block escapes to std::terminate and the game
        // aborts (STATUS_FATAL_APP_EXIT, mod.dll in the abort chain). So the
        // handler speaks only in fixed buffers + snprintf, and logging is
        // itself wrapped. The tiles and manifest are already on disk, so a
        // caught failure is recoverable: "lumen stitch <manifest>".
        lumen::stitch::Options opt;
        opt.out = manifest;
        opt.out.replace_extension(".tiff");
        char errbuf[512];
        errbuf[0] = '\0';
        std::string run_err; // empty: no allocation; run() fills it on soft failures
        bool ok = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            run_err.clear();
            try {
                ok = lumen::stitch::run(manifest, opt, run_err,
                    [](uint32_t p) { g_stitch_percent = p; });
                break;
            } catch (const std::bad_alloc&) {
                if (attempt == 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }
                std::snprintf(errbuf, sizeof(errbuf),
                    "Not enough free memory to stitch - the parts are saved. "
                    "Close other programs and run: lumen stitch lumen-%llu.json",
                    static_cast<unsigned long long>(job_id));
            } catch (const std::exception& e) {
                std::snprintf(errbuf, sizeof(errbuf),
                    "Stitch stopped: %.180s - the parts are saved; "
                    "run lumen stitch on the manifest.",
                    e.what());
                break;
            } catch (...) {
                std::snprintf(errbuf, sizeof(errbuf),
                    "Stitch stopped by an unknown error - the parts are saved; "
                    "run lumen stitch on the manifest.");
                break;
            }
        }
        if (!ok && errbuf[0] == '\0')
            std::snprintf(errbuf, sizeof(errbuf), "%.400s", run_err.c_str());
        if (!ok) {
            // Do not leave a truncated BigTIFF that looks complete.
            std::error_code ec;
            std::filesystem::remove(opt.out, ec);
            try {
                mods::log::warn("lumen: stitch job {} failed: {}", job_id, errbuf);
            } catch (...) {
            }
        }
        std::lock_guard lock(g_stitch_mtx);
        try {
            g_stitch_done = ok ? opt.out.filename().string() : std::string(errbuf);
        } catch (...) {
            // Allocation failed even here: report generically instead.
            g_stitch_done = "Stitch failed (low memory) - the parts are saved; "
                            "run lumen stitch on the manifest.";
        }
        g_stitch_done_ok = ok;
        g_stitch_running = false;
    });
    char body[192];
    std::snprintf(body, sizeof(body),
        "Stitching lumen-%" PRIu64 " in the background - carry on playing.", job_id);
    push_photo_toast("Lumen Photo Mode", body, "info");
    mods::log::info("lumen: auto-stitch started for job {}", job_id);
}

ModResult begin_capture(const char* name_override, bool for_job) {
    if (g_capture.phase != CAPTURE_NONE)
        return MOD_OK; // one shot at a time
    g_capture.settle_left = static_cast<int>(cfg_int(g_var_settle_frames, 4));
    g_capture.stall_frames = 0;
    g_capture.for_job = for_job;
    g_capture.name.clear();
    if (name_override) {
        const size_t n = std::strlen(name_override);
        g_capture.name.assign(name_override, name_override + n + 1);
    }
    g_capture.phase = CAPTURE_WAIT_SETTLE;
    return MOD_OK;
}

ModResult request_capture() {
    if (!g_photo.active) {
        push_photo_toast("Lumen Photo Mode", "Enter photo mode first (F1).", "warning");
        return MOD_ERROR;
    }
    if (g_job.active) {
        push_photo_toast("Lumen Photo Mode", "A capture job is running.", "warning");
        return MOD_OK;
    }
    return begin_capture(nullptr, false);
}

// ---------------------------------------------------------------- gigapixel job

void abort_job(bool notify) {
    if (!g_job.active)
        return;
    g_job.active = false;
    strip_end();
    g_job.tiles.clear();
    g_job.idx = 0;
    g_job.json.clear();
    g_photo.fov_override = 0;
    g_photo.eye_off[0] = g_photo.eye_off[1] = g_photo.eye_off[2] = 0.0f;
    if (g_capture.phase != CAPTURE_NONE) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
    }
    if (notify)
        push_photo_toast("Lumen Photo Mode", "Capture job canceled.", "warning");
}

// A capture attempt failed outright. For job tiles this ends the job: the
// same tile would otherwise be re-armed every frame (device-lost style
// failures never heal), spamming toasts and never completing. The manifest
// is only written on full completion, so an aborted job stays stitchable
// only from its complete tile set - never from a silently partial one.
void fail_capture(const char* why) {
    const bool for_job = g_capture.for_job;
    reset_capture_buffers();
    g_capture.phase = CAPTURE_NONE;
    g_capture.for_job = false;
    push_photo_toast("Lumen Photo Mode", why, "error");
    if (for_job)
        abort_job(false);
}

// Guard shared by every multi-part capture kind.
bool job_startable() {
    // Every rejection speaks up: a key that reached us but was refused must
    // say why, so a silent job start is never mistaken for a dead hotkey.
    if (!g_photo.active || g_job.active || g_capture.phase != CAPTURE_NONE) {
        if (!g_photo.active)
            push_photo_toast("Lumen Photo Mode", "Enter photo mode first (F1).", "warning");
        else if (g_job.active)
            push_photo_toast("Lumen Photo Mode", "A capture job is already running.", "warning");
        else
            push_photo_toast("Lumen Photo Mode", "A capture is still finishing - try again.",
                "warning");
        return false;
    }
    if (!folder_ok()) {
        push_photo_toast("Lumen Photo Mode", "Set a capture folder in the Lumen panel first.",
            "warning");
        return false;
    }
    return true;
}

// Shared kickoff: everything is decided after the viewport-size probe.
// Third-party interop: Twilit Essentials (com.dusklight.twilit_essentials)
// adds effects that ANIMATE EVERY RENDER FRAME and keep frame history - Wind
// on foliage/cloth, 3D grass geometry, volumetric clouds + cloud shadows,
// and Color & Lens film grain / chromatic aberration / vignette (their own
// WGSL + temporal caches). No actor pause can stop them: their time source
// is the render frame itself. Lumen cannot pause another mod, but it can
// detect the package next to ours and say so once, so the user knows which
// toggles make multi-tile jobs seamless.
bool twilit_essentials_present() {
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;
    std::vector<std::string> roots;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"))
        roots.push_back(std::string(appdata) + "\\TwilitRealm\\Dusklight\\mods");
#else
    const char* home = std::getenv("HOME");
    if (home) {
#ifdef __APPLE__
        roots.push_back(std::string(home) + "/Library/Application Support/TwilitRealm/Dusklight/mods");
#else
        roots.push_back(std::string(home) + "/.local/share/TwilitRealm/Dusklight/mods");
#endif
    }
#endif
    bool found = false;
    for (const std::string& dir : roots) {
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
            std::string n = de.path().filename().string();
            for (char& c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find("twilit_essentials") != std::string::npos) {
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    cached = found ? 1 : 0;
    if (found)
        mods::log::info("lumen: Twilit Essentials detected - per-frame effects will differ "
                        "between tiles unless disabled in its Graphics menu");
    return found;
}

void begin_job(JobState::Kind kind, uint32_t S) {
    g_job.active = true;
    g_job.build_pending = true; // need the scene color size before aiming tiles
    g_job.probe_frames = 0;
    g_job.probe_w = g_job.probe_h = 0;
    g_job.rebuilds = 0;
    g_job.kind = kind;
    g_job.S = S;
    g_job.tiles.clear();
    g_job.idx = 0;
    g_job.json.clear();
    g_job.id = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    strip_begin(); // hold screen-space/temporal effects off for the whole job
    // One-time heads-up when a per-frame-animated companion mod is present.
    if (kind != JobState::STEREO && !g_compat_hinted &&
        cfg_bool(g_var_essentials_hint, true) && twilit_essentials_present()) {
        g_compat_hinted = true;
        push_photo_toast("Lumen Photo Mode",
            "Twilit Essentials detected: close its Photo Mode (its haze, filters and "
            "letterbox bake into every tile), and turn off Wind / 3D Grass / Clouds in "
            "its Graphics menu - they animate every frame, so tiles seam.", "warning");
    }
}

void start_gigapixel() {
    if (!job_startable())
        return;
    const int64_t S = cfg_int(g_var_capture_scale, 4);
    if (S < 1) {
        push_photo_toast("Lumen Photo Mode",
            "Tiles per side must be 1 or more (Lumen panel).", "warning");
        return;
    }
    if (S > 33) {
        push_photo_toast("Lumen Photo Mode",
            "Tiles-per-side above 33 exceeds the supported maximum.", "warning");
        return;
    }
    if (S > 16) {
        push_photo_toast("Lumen Photo Mode",
            "Over 16x uses the opt-in high-cost path: thousands of frames.", "info");
    }
    begin_job(JobState::GRID, static_cast<uint32_t>(S));
    mods::log::info("lumen: gigapixel job {} started ({}x{} tiles)", g_job.id, S, S);
}

// Ansel's 360: six axis-aligned FOV-90 faces at the frozen eye (fixed pivot),
// stitched offline into an equirectangular panorama by `lumen stitch`.
void start_360() {
    if (!job_startable())
        return;
    begin_job(JobState::CUBE360, 0);
    mods::log::info("lumen: 360 job {} started (6 faces)", g_job.id);
}

// Ansel's stereo: left / center / right views of the same frozen scene, eyes
// shifted along the camera right axis by +/- ipd/2 (fixed convergence point).
void start_stereo() {
    if (!job_startable())
        return;
    begin_job(JobState::STEREO, 0);
    mods::log::info("lumen: stereo job {} started (3 views)", g_job.id);
}

// The probed v2.0.3 scene target is BGRA8Unorm (format 27) - aurora's swap-
// chain format. Both 8-bit orders are handled losslessly by the readback
// shader (it swaps components when the format is BGRA); float/HDR formats
// would quantize, so they stay flagged as a known gap.
void warn_if_not_rgba8() {
    if (g_job.color_format != WGPUTextureFormat_RGBA8Unorm &&
        g_job.color_format != WGPUTextureFormat_BGRA8Unorm)
        mods::log::warn("lumen: scene color format {} - capture quantizes to LDR 8-bit; "
                        "HDR readback not wired for this adapter",
            static_cast<unsigned>(g_job.color_format));
}

// Six cube faces (tilemath::cube360_faces - shared with the stitcher). The
// manifest master block carries the panorama center (capture yaw) plus the
// viewport size; width/height describe the 4x2 atlas slots, which are
// bookkeeping only for this layout.
void build_cube_tiles(uint32_t vw, uint32_t vh) {
    g_job.vw = vw;
    g_job.vh = vh;
    warn_if_not_rgba8();
    tilemath::FacePose fp[6];
    tilemath::cube360_faces(g_photo.yaw, fp);
    char name[160];
    for (const tilemath::FacePose& f : fp) {
        JobTileDef def;
        def.row = f.row;
        def.col = f.col;
        def.yaw = f.yaw;
        def.pitch = f.pitch;
        // Pole faces are tilted off exact vertical (the game's lookAt cannot
        // represent it) and carry the matching wider fovy.
        def.fovy = f.row == 1 ? tilemath::kCube360PoleFovy : tilemath::kCube360Fovy;
        def.x0 = static_cast<uint32_t>(f.col) * vw;
        def.y0 = static_cast<uint32_t>(f.row) * vh;
        def.x1 = def.x0 + vw;
        def.y1 = def.y0 + vh;
        std::snprintf(name, sizeof(name), "lumen-%" PRIu64 "-r%u-c%u.png", g_job.id,
            static_cast<unsigned>(f.row), static_cast<unsigned>(f.col));
        def.name.assign(name, name + std::strlen(name) + 1);
        g_job.tiles.push_back(std::move(def));
    }
    char head[320];
    std::snprintf(head, sizeof(head),
        "{\"lumenJob\":1,\"layout\":\"cube360\",\"scale\":1,\"colorFormat\":%u,"
        "\"master\":{\"yaw\":%.9g,\"pitch\":0,\"fovy\":%.9g,\"vw\":%" PRIu32 ",\"vh\":%" PRIu32
        ",\"width\":%" PRIu32 ",\"height\":%" PRIu32 "},\"tiles\":[",
        static_cast<unsigned>(g_job.color_format), g_photo.yaw, tilemath::kCube360Fovy, vw, vh,
        4 * vw, 2 * vh);
    g_job.json = head;
    g_job.build_pending = false;
    mods::log::info("lumen: 360 job {} aiming 6 faces at viewport {}x{}", g_job.id, vw, vh);
}

// Stereo trio: left/center/right at native FOV, side-by-side slots on one
// master row. L/R eyes shift along the camera RIGHT axis by +/- ipd/2; the
// master block records the center view, each tile its own eye offset, so the
// stitcher lays them out 1:1 and future tools can redress (anaglyph, shift).
void build_stereo_tiles(uint32_t vw, uint32_t vh) {
    g_job.vw = vw;
    g_job.vh = vh;
    warn_if_not_rgba8();
    const double ipd = static_cast<double>(cfg_int(g_var_stereo_ipd, 100)) / 100.0;
    const double yaw = g_photo.yaw;
    // Camera right in world axes (Y-up): forward is (sin yaw, 0, cos yaw).
    const double rx = std::cos(yaw), rz = -std::sin(yaw);
    const double half = ipd * 0.5;
    // row 0: left, center, right
    const double signs[3] = {-1.0, 0.0, 1.0};
    char name[160];
    for (int i = 0; i < 3; ++i) {
        JobTileDef def;
        def.row = 0;
        def.col = i;
        def.yaw = yaw;
        def.pitch = g_photo.pitch;
        def.fovy = g_photo.fovy;
        def.ox = signs[i] * half * rx;
        def.oz = signs[i] * half * rz;
        def.x0 = static_cast<uint32_t>(i) * vw;
        def.y0 = 0;
        def.x1 = def.x0 + vw;
        def.y1 = vh;
        std::snprintf(name, sizeof(name), "lumen-%" PRIu64 "-r0-c%u.png", g_job.id,
            static_cast<unsigned>(i));
        def.name.assign(name, name + std::strlen(name) + 1);
        g_job.tiles.push_back(std::move(def));
    }
    char head[320];
    std::snprintf(head, sizeof(head),
        "{\"lumenJob\":1,\"layout\":\"stereo\",\"scale\":1,\"colorFormat\":%u,"
        "\"master\":{\"yaw\":%.9g,\"pitch\":%.9g,\"fovy\":%.9g,\"vw\":%" PRIu32 ",\"vh\":%" PRIu32
        ",\"width\":%" PRIu32 ",\"height\":%" PRIu32 "},\"tiles\":[",
        static_cast<unsigned>(g_job.color_format), yaw, static_cast<double>(g_photo.pitch),
        static_cast<double>(g_photo.fovy), vw, vh, 3 * vw, vh);
    g_job.json = head;
    g_job.build_pending = false;
    mods::log::info("lumen: stereo job {} at ipd {} viewport {}x{}", g_job.id, ipd, vw, vh);
}

// Build the tile grid once the scene color size is known (probe frame).
void build_job_tiles(uint32_t vw, uint32_t vh) {
    if (g_job.kind == JobState::CUBE360) {
        build_cube_tiles(vw, vh);
        return;
    }
    if (g_job.kind == JobState::STEREO) {
        build_stereo_tiles(vw, vh);
        return;
    }
    const uint32_t S0 = g_job.S;
    if (!S0) {
        abort_job(true);
        return;
    }
    // Master dimension cap (Ansel's gigapixel ceiling): the master image is
    // vw*S x vh*S, so clamp tiles-per-side to keep it inside 65536 px. Known
    // here, before a single tile renders; nothing wasted.
    uint32_t cap = 65536u / vw;
    if (uint32_t caph = 65536u / vh; caph < cap)
        cap = caph;
    if (cap < 2)
        cap = 2;
    const uint32_t S = S0 < cap ? S0 : cap;
    if (S != S0) {
        char body[192];
        std::snprintf(body, sizeof(body),
            "Scale limited to %" PRIu32 "x: %" PRIu32 "x on a %" PRIu32 "x%" PRIu32
            " viewport would exceed the 65536 px master-image cap.",
            S, S0, vw, vh);
        push_photo_toast("Lumen Photo Mode", body, "info");
        g_job.S = S;
    }
    tilemath::Master m;
    m.yaw = g_photo.yaw;
    m.pitch = g_photo.pitch;
    m.fovy = g_photo.fovy;
    m.vw = vw;
    m.vh = vh;
    m.width = vw * S;
    m.height = vh * S;
    g_job.vw = vw;
    g_job.vh = vh;
    warn_if_not_rgba8();
    char name[160];
    for (uint32_t r = 0; r < S; ++r) {
        for (uint32_t c = 0; c < S; ++c) {
            const uint32_t x0 = c * vw, y0 = r * vh;
            const uint32_t x1 = x0 + vw, y1 = y0 + vh;
            const tilemath::Tile t = tilemath::make_tile(m, x0, y0, x1, y1,
                static_cast<int>(r), static_cast<int>(c));
            JobTileDef def;
            def.row = static_cast<int>(r);
            def.col = static_cast<int>(c);
            def.yaw = t.yaw;
            def.pitch = t.pitch;
            def.fovy = t.fovy;
            def.x0 = x0;
            def.y0 = y0;
            def.x1 = x1;
            def.y1 = y1;
            std::snprintf(name, sizeof(name), "lumen-%" PRIu64 "-r%u-c%u.png", g_job.id, r, c);
            def.name.assign(name, name + std::strlen(name) + 1);
            g_job.tiles.push_back(std::move(def));
        }
    }
    char head[320];
    std::snprintf(head, sizeof(head),
        "{\"lumenJob\":1,\"scale\":%" PRIu32 ",\"colorFormat\":%u,\"master\":{\"yaw\":%.9g,"
        "\"pitch\":%.9g,\"fovy\":%.9g,\"vw\":%" PRIu32 ",\"vh\":%" PRIu32 ",\"width\":%" PRIu32
        ",\"height\":%" PRIu32 "},\"tiles\":[",
        S, static_cast<unsigned>(g_job.color_format), m.yaw, m.pitch, m.fovy, vw, vh, m.width,
        m.height);
    g_job.json = head;
    g_job.build_pending = false;
}

// Apply the next tile's pose and start its capture. The eye never moves
// during a job (all tiles share the master eye - pure rotation).
void advance_job() {
    if (g_job.idx >= g_job.tiles.size())
        return;
    const JobTileDef& t = g_job.tiles[g_job.idx];
    g_photo.yaw = static_cast<float>(t.yaw);
    g_photo.pitch = static_cast<float>(t.pitch);
    g_photo.fov_override = static_cast<float>(t.fovy);
    // Eye stays put for grid/360 (pure rotation); stereo shifts it per view.
    g_photo.eye_off[0] = static_cast<float>(t.ox);
    g_photo.eye_off[1] = static_cast<float>(t.oy);
    g_photo.eye_off[2] = static_cast<float>(t.oz);
    begin_capture(t.name.data(), true);
}

// Manifest entry for the tile that just completed.
void record_job_tile() {
    const JobTileDef& t = g_job.tiles[g_job.idx];
    char entry[448];
    std::snprintf(entry, sizeof(entry),
        "%s{\"file\":\"%s\",\"row\":%d,\"col\":%d,\"yaw\":%.9g,\"pitch\":%.9g,"
        "\"fovy\":%.9g,\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"eye\":[%.9g,%.9g,%.9g],"
        "\"rect\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]}",
        g_job.idx ? "," : "", t.name.data(), t.row, t.col, t.yaw, t.pitch, t.fovy, g_capture.width,
        g_capture.height, t.ox, t.oy, t.oz, t.x0, t.y0, t.x1, t.y1);
    g_job.json += entry;
    ++g_job.idx;
    if (g_job.idx < g_job.tiles.size())
        return;

    // Job complete: write the manifest and hand off to the stitcher CLI.
    g_job.json += "]}";
    char manifest_name[160];
    std::snprintf(manifest_name, sizeof(manifest_name), "lumen-%" PRIu64 ".json", g_job.id);
    const char* child = nullptr;
    bool wrote = false;
    if (svc_file->create_child(mod_ctx, g_folder_location, manifest_name, &child) == MOD_OK &&
        child) {
        char child_copy[1024];
        std::snprintf(child_copy, sizeof(child_copy), "%s", child);
        wrote = svc_file->write_all(mod_ctx, child_copy, g_job.json.data(), g_job.json.size()) ==
                MOD_OK;
    }
    const uint64_t id = g_job.id;
    const size_t tiles = g_job.tiles.size();
    const auto kind = g_job.kind;
    g_job.active = false;
    strip_end();
    g_job.tiles.clear();
    g_job.json.clear();
    g_photo.fov_override = 0;
    g_photo.eye_off[0] = g_photo.eye_off[1] = g_photo.eye_off[2] = 0.0f;
    if (wrote) {
        char body[256];
        std::snprintf(body, sizeof(body),
            "%s done: %zu %s saved. Press F1 to return to the game.",
            kind == JobState::CUBE360   ? "360 panorama"
                : kind == JobState::STEREO ? "Stereo"
                                           : "Gigapixel",
            tiles,
            kind == JobState::CUBE360   ? "faces"
                : kind == JobState::STEREO ? "views"
                                           : "tiles");
        push_photo_toast("Lumen Photo Mode", body, "success", 5000);
        mods::log::info("lumen: {} job {} complete ({} parts)",
            kind == JobState::CUBE360   ? "360"
                : kind == JobState::STEREO ? "stereo"
                                           : "gigapixel",
            id, tiles);
        start_auto_stitch(id);
    } else {
        push_photo_toast("Lumen Photo Mode",
            kind == JobState::CUBE360   ? "360 faces saved but the manifest could not be written."
                : kind == JobState::STEREO ? "Stereo views saved but the manifest could not be written."
                                           : "Gigapixel tiles saved but the manifest could not be written.",
            "error");
    }
}

void enter_photo_mode() {
    if (g_photo.active)
        return;
    if (!g_feat_capture) {
        push_photo_toast("Lumen Photo Mode",
            "Capture is unavailable in this Dusklight build (graphics stage hooks failed).",
            "error");
        return;
    }
    g_photo.active = true;
    g_photo.seeded = false;
    g_photo.fovy = static_cast<float>(cfg_int(g_var_photo_fov, 90));

    CameraOperatorDesc desc = CAMERA_OPERATOR_DESC_INIT;
    desc.debug_name = "lumen_photo";
    desc.priority = 1000;
    desc.operate = photo_operate;
    if (svc_camera->register_camera_operator(mod_ctx, &desc, &g_operator) != MOD_OK) {
        g_operator = 0;
        mods::log::warn("lumen: camera operator registration failed");
    }

    // The freeze is the game's own global actor pause (the `freeze` console
    // command's byte). No clock surgery: zeroing the shared FrameTiming wedged
    // the game loop on 2026-11-25's builds.
    g_photo.frozen = g_feat_world_freeze;
    world_freeze_begin();
    g_photo_remind_at = std::chrono::steady_clock::now();

    char body[256];
    if (g_world_pin.pinned)
        std::snprintf(body, sizeof(body),
            "Photo mode on - the world is frozen. F7 capture, F12 gigapixel, "
            "Shift+F12 360, Shift+F7 stereo. Press F1 to return to the game.");
    else
        std::snprintf(body, sizeof(body),
            "Photo mode on (world freeze unavailable in this build). Press F1 to return to the game.");
    push_photo_toast("Lumen Photo Mode", body, "info", 4000);
    if (!cfg_bool(g_var_seen_controls, false)) {
        if (g_var_seen_controls)
            svc_config->set_bool(mod_ctx, g_var_seen_controls, true);
        push_controls_dialog();
    }
    mods::log::info("lumen: photo mode on (world freeze={})", g_world_pin.pinned);
}

void exit_photo_mode() {
    if (!g_photo.active)
        return;
    if (g_job.active)
        abort_job(false);
    g_photo.active = false;
    g_photo.frozen = false;
    world_freeze_end();
    // Keep the operator registered a little longer: it spends the next few
    // frames writing the game's original pose back (photo_operate's restore
    // branch), and mod_update unregisters it once that handoff is done.
    g_photo.restore_frames = g_photo.have_saved ? 8 : 0;
    g_photo.seeded = false;
    if (g_capture.phase != CAPTURE_NONE) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
    }
    // Short and plain: this is the "you are back" signal; a long toast here
    // reads like photo mode is still on (reported 2026-11-26). The overlay
    // has no dismiss API, so bounded duration is the only lever.
    push_photo_toast("Lumen Photo Mode", "Photo mode off - back to the game.", "info", 2000);
    mods::log::info("lumen: photo mode off");
}

// ---------------------------------------------------------------- input (Windows)

#ifdef _WIN32
bool key_held(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Hotkey map vs the game's own F-keys (dusk::hotkeys): F2-F6 and F8-F11 are
// taken (process mgmt, debug overlays, save editor, state share, debug
// camera, audio debug, TOGGLE FULLSCREEN). Hijacking them caused "windowed
// mode + stuck zoomed camera": F11 flipped the game's fullscreen setting and
// F9 turned on the game's debug camera under us. Lumen uses only free keys:
//   F1 photo mode, F7 capture, F12 gigapixel,
//   Shift+F7 stereo trio, Shift+F12 360 panorama.
// Alt+ variants of every chord work too - the poll reads the F-key edge with
// any modifiers held - because third-party overlays (Steam's screenshot
// hotkey is plain F12) register bare F-keys and can eat them before any app
// sees them. Never bind Alt+F1: the game hides its ImGui console on Shift+F1
// and some IME/menu paths use Alt+F1.
bool g_prev_f1 = false;
bool g_prev_f7 = false;
bool g_prev_f12 = false;

void poll_hotkeys() {
    const bool f1 = key_held(VK_F1);
    if (f1 && !g_prev_f1) {
        if (g_photo.active)
            exit_photo_mode();
        else
            enter_photo_mode();
    }
    g_prev_f1 = f1;

    const bool f7 = key_held(VK_F7);
    if (f7 && !g_prev_f7) {
        if (key_held(VK_SHIFT))
            start_stereo();
        else
            request_capture();
    }
    g_prev_f7 = f7;

    const bool f12 = key_held(VK_F12);
    if (f12 && !g_prev_f12) {
        if (key_held(VK_SHIFT))
            start_360();
        else
            start_gigapixel();
    }
    g_prev_f12 = f12;
}

void apply_flight_input(float dt) {
    if (!g_photo.active)
        return;
    if (g_job.active) {
        // Tiles must share one eye and one master frustum: no piloting while
        // a gigapixel job drives the camera.
        return;
    }
    float speed = static_cast<float>(cfg_int(g_var_move_speed, 100));
    if (key_held(VK_SHIFT))
        speed *= 4.0f;
    const float look = static_cast<float>(cfg_int(g_var_look_speed, 45)) * kDegToRad;

    if (key_held(VK_LEFT)) g_photo.yaw += look * dt;
    if (key_held(VK_RIGHT)) g_photo.yaw -= look * dt;
    if (key_held(VK_UP)) g_photo.pitch += look * dt;
    if (key_held(VK_DOWN)) g_photo.pitch -= look * dt;
    if (g_photo.pitch > 1.4835f) g_photo.pitch = 1.4835f;  // 85 deg: the game's
    if (g_photo.pitch < -1.4835f) g_photo.pitch = -1.4835f; // lookAt dies near 90
    if (key_held(VK_PRIOR)) g_photo.fovy += 30.0f * dt;
    if (key_held(VK_NEXT)) g_photo.fovy -= 30.0f * dt;
    if (g_photo.fovy < 5.0f) g_photo.fovy = 5.0f;
    if (g_photo.fovy > 170.0f) g_photo.fovy = 170.0f;

    const float fx = std::cosf(g_photo.pitch) * std::sinf(g_photo.yaw);
    const float fy = std::sinf(g_photo.pitch);
    const float fz = std::cosf(g_photo.pitch) * std::cosf(g_photo.yaw);
    const float rx = std::cosf(g_photo.yaw);
    const float rz = -std::sinf(g_photo.yaw);

    float mx = 0, my = 0, mz = 0;
    if (key_held('W')) { mx += fx; my += fy; mz += fz; }
    if (key_held('S')) { mx -= fx; my -= fy; mz -= fz; }
    if (key_held('D')) { mx += rx; mz += rz; }
    if (key_held('A')) { mx -= rx; mz -= rz; }
    if (key_held('E')) my += 1.0f;
    if (key_held('Q')) my -= 1.0f;
    const float len = std::sqrtf(mx * mx + my * my + mz * mz);
    if (len > 0.001f) {
        const float s = speed * dt / len;
        g_photo.eye[0] += mx * s;
        g_photo.eye[1] += my * s;
        g_photo.eye[2] += mz * s;
    }
}
#else
void poll_hotkeys() {}
void apply_flight_input(float) {}
#endif

// ---------------------------------------------------------------- capture pipeline

constexpr char kReadbackShader[] = R"wgsl(
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> dst : array<u32>;
struct P { w:u32, h:u32, swap_rb:u32, sp0:u32 };
@group(0) @binding(2) var<uniform> p : P;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= p.w || gid.y >= p.h) {
        return;
    }
    let c = textureLoad(src, vec2<i32>(i32(gid.x), i32(gid.y)), 0);
    let q = vec4<u32>(vec4<i32>(clamp(c, vec4<f32>(0.0), vec4<f32>(1.0)) * vec4<f32>(255.0) + vec4<f32>(0.5)));
    var r = q.x;
    var b = q.z;
    if (p.swap_rb != 0u) {
        r = q.z;
        b = q.x;
    }
    // Screenshot alpha is opaque regardless of what the scene buffer stored:
    // the game leaves scene-color alpha at 0, and a zero-alpha PNG renders
    // blank in every viewer (and premultiplies to black in GDI+/WIC).
    dst[gid.y * p.w + gid.x] = r | (q.y << 8u) | (b << 16u) | 0xFF000000u;
}
)wgsl";

void ensure_pipeline(WGPUDevice device) {
    if (g_capture.pipeline)
        return;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {kReadbackShader, WGPU_STRLEN};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"lumen_readback", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &moduleDesc);
    if (!module) {
        mods::log::error("lumen: readback shader module creation failed");
        return;
    }
    WGPUComputePipelineDescriptor pipeDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pipeDesc.label = {"lumen_readback", WGPU_STRLEN};
    pipeDesc.layout = nullptr; // auto
    pipeDesc.compute.module = module;
    pipeDesc.compute.entryPoint = {"main", WGPU_STRLEN};
    g_capture.pipeline = wgpuDeviceCreateComputePipeline(device, &pipeDesc);
    wgpuShaderModuleRelease(module);
    if (!g_capture.pipeline)
        mods::log::error("lumen: readback compute pipeline creation failed");
}

struct ReadbackPayload {
    WGPUTextureView view;    // resolved scene color (valid this frame)
    WGPUBuffer storage;      // compute output (mod-owned)
    WGPUBuffer staging;      // map-read destination (mod-owned)
    uint32_t bytes;
    uint32_t uniform_offset; // dims slot inside the frame uniform stream
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(ReadbackPayload) <= 128);

// Render worker thread: dispatch the copy, then storage -> staging on the frame encoder.
void on_readback_compute(ModContext*, const GfxComputeContext* compute, const void* payload,
    size_t payload_size, void*) {
    if (payload_size != sizeof(ReadbackPayload) || !g_capture.pipeline)
        return;
    ReadbackPayload p;
    std::memcpy(&p, payload, sizeof(p));

    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(g_capture.pipeline, 0);
    if (!bgl)
        return;
    WGPUBindGroupEntry entries[3] = {
        WGPUBindGroupEntry{.binding = 0, .buffer = nullptr, .offset = 0,
            .size = WGPU_WHOLE_SIZE, .textureView = p.view},
        WGPUBindGroupEntry{.binding = 1, .buffer = p.storage, .offset = 0,
            .size = p.bytes, .textureView = nullptr},
        WGPUBindGroupEntry{.binding = 2, .buffer = compute->uniform_buffer,
            .offset = p.uniform_offset, .size = 16, .textureView = nullptr},
    };
    WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgDesc.label = {"lumen_readback", WGPU_STRLEN};
    bgDesc.layout = bgl;
    bgDesc.entryCount = 3;
    bgDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(compute->device, &bgDesc);
    wgpuBindGroupLayoutRelease(bgl);
    if (!bindGroup)
        return;

    WGPUComputePassDescriptor passDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"lumen_readback", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(compute->encoder, &passDesc);
    if (pass) {
        wgpuComputePassEncoderSetPipeline(pass, g_capture.pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, (p.width + 7u) / 8u, (p.height + 7u) / 8u, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        static int s_dispatch_logged = 0;
        if (s_dispatch_logged < 4) {
            ++s_dispatch_logged;
            mods::log::info("lumen: readback dispatched inside frame encoder ({}x{})",
                p.width, p.height);
        }
    }
    wgpuCommandEncoderCopyBufferToBuffer(compute->encoder, p.storage, 0, p.staging, 0, p.bytes);
    wgpuBindGroupRelease(bindGroup);
}

GfxComputeTypeHandle g_compute_type = 0;

// The viewport changed while a job was mid-flight (the game's internal
// resolution is "Auto" by default: the present/EFB size tracks the window,
// and menu/HUD present-target switches can flicker it). Rather than lose the
// job, re-freeze the grid at the new size: tiles restart from tile 0 under a
// fresh id (old partial PNGs have no manifest, so the stitcher ignores them).
// Bounded retries: a scene that resizes on every tile is unwinnable, and
// then the honest answer is to fix the game's setting.
bool rebuild_job_at_size(uint32_t w, uint32_t h) {
    if (g_job.rebuilds >= 3)
        return false;
    ++g_job.rebuilds;
    mods::log::warn("lumen: viewport changed mid-job {}x{} -> {}x{}; rebuilding grid (attempt {})",
        g_job.vw, g_job.vh, w, h, g_job.rebuilds);
    g_capture.phase = CAPTURE_NONE;
    g_capture.for_job = false;
    g_capture.name.clear();
    g_job.tiles.clear();
    g_job.idx = 0;
    g_job.json.clear();
    g_job.build_pending = true;
    g_job.probe_frames = 0;
    g_job.probe_w = w; // seed the probe: one confirming frame, then aim
    g_job.probe_h = h;
    g_job.id = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return true;
}

void issue_capture(WGPUDevice device) {
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    GfxResolvedTargets targets = GFX_RESOLVED_TARGETS_INIT;
    g_gfx_stage = "resolve_pass";
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &targets) != MOD_OK || !targets.color) {
        fail_capture("Capture failed: no scene color snapshot.");
        return;
    }
    const uint32_t w = targets.width;
    const uint32_t h = targets.height;
    const uint32_t bytes = w * h * 4u;
    // Job tiles must share the geometry frozen at job start: if the scene
    // size drifts mid-job (dynamic resolution scaling), stop rather than
    // mix tile sizes in one manifest.
    if (g_capture.for_job && g_job.active && g_job.vw != 0 &&
        (w != g_job.vw || h != g_job.vh)) {
        if (rebuild_job_at_size(w, h))
            return;
        fail_capture("Capture failed: scene resolution kept changing. Set the game's "
                     "Internal Resolution to a fixed value (not Auto) and retry.");
        return;
    }
    ensure_pipeline(device);
    if (!g_capture.pipeline) {
        fail_capture("Capture failed: compute pipeline unavailable.");
        return;
    }

    g_gfx_stage = "create_buffers";
    if (!ensure_capture_buffers(device, bytes)) {
        fail_capture("Capture failed: buffer allocation failed.");
        return;
    }
    // Dims ride the frame's shared uniform stream; binding 2 resolves to that
    // buffer's pushed slot inside the compute context. swap_rb: measured, not
    // assumed (2026-11-26): the same scene captured and photographed showed
    // green matching to 3 decimals while red and blue were exactly exchanged
    // (capture R med .349 vs screen B .345; capture B .475 vs screen R .467).
    // Dawn's D3D backend samples even bgra8unorm targets in logical
    // (R,G,B,A) order, so no swap is correct here. The old format-based swap
    // inverted the picture (warm scenes read blue-grey - the "dark capture"
    // complaint). The toggle is kept for backends that ever disagree.
    const uint32_t swap_rb = cfg_bool(g_var_swap_rb, false) ? 1u : 0u;
    const uint32_t dims[4] = {w, h, swap_rb, 0};
    GfxRange uniformRange = {};
    g_gfx_stage = "push_uniform";
    if (svc_gfx->push_uniform(mod_ctx, dims, sizeof(dims), &uniformRange) != MOD_OK) {
        fail_capture("Capture failed: uniform stream rejected.");
        return;
    }

    ReadbackPayload payload{targets.color, g_capture.storage, g_capture.staging, bytes,
        uniformRange.offset, w, h};
    g_gfx_stage = "push_compute";
    if (svc_gfx->push_compute(mod_ctx, g_compute_type, &payload, sizeof(payload)) != MOD_OK) {
        fail_capture("Capture failed: compute task rejected.");
        return;
    }
    g_gfx_stage = "capture_issued_log";
    // Safety: fmt THROWS ("string pointer is null") on a null char* - a bare
    // F7 capture carries an empty name vector, and this callback runs
    // unguarded in the game's frame loop (measured 2026-11-26: that throw
    // killed the game pre-shield and spun the hook forever after).
    mods::log::info("lumen: capture issued {} ({}x{}, job={})",
        g_capture.name.empty() ? "(auto)" : g_capture.name.data(), w, h,
        g_capture.for_job ? 1 : 0);
    g_capture.width = w;
    g_capture.height = h;
    g_capture.phase = CAPTURE_WAIT_COPY;
    g_gfx_stage = "idle";
}

// ---------------------------------------------------------------- png sink

void png_write_cb(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

void finish_capture_file() {
    const uint64_t bytes = static_cast<uint64_t>(g_capture.width) * g_capture.height * 4ull;
    const bool for_job = g_capture.for_job;
    std::vector<char> name = g_capture.name;
    g_capture.name.clear();
    g_capture.for_job = false;
    const void* mapped = wgpuBufferGetConstMappedRange(g_capture.staging, 0, bytes);
    if (!mapped) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: readback unavailable.", "error");
        if (for_job)
            abort_job(true);
        return;
    }
    std::vector<uint8_t> png;
    const int ok = stbi_write_png_to_func(png_write_cb, &png, static_cast<int>(g_capture.width),
        static_cast<int>(g_capture.height), 4, mapped, static_cast<int>(g_capture.width * 4));
    // Keep the buffer pair for the next capture (reuse pool); only the
    // staging mapping must drop before the next MapAsync.
    if (g_capture.staging &&
        wgpuBufferGetMapState(g_capture.staging) == WGPUBufferMapState_Mapped)
        wgpuBufferUnmap(g_capture.staging);
    g_capture.phase = CAPTURE_NONE;
    auto fail = [&](const char* why) {
        push_photo_toast("Lumen Photo Mode", why, "error");
        if (for_job)
            abort_job(true);
    };
    if (!ok || png.empty()) {
        fail("Capture failed: PNG encode error.");
        return;
    }
    if (name.empty()) {
        const long long now = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        char default_name[128];
        std::snprintf(default_name, sizeof(default_name), "lumen-%lld-%ux%u.png", now,
            g_capture.width, g_capture.height);
        name.assign(default_name, default_name + std::strlen(default_name) + 1);
    }
    if (!folder_ok()) {
        push_photo_toast("Lumen Photo Mode", "Set a capture folder in the Lumen panel first.",
            "warning");
        if (for_job)
            abort_job(true);
        return;
    }
    const char* child = nullptr;
    if (svc_file->create_child(mod_ctx, g_folder_location, name.data(), &child) != MOD_OK ||
        !child) {
        fail("Capture failed: could not create file.");
        return;
    }
    char child_copy[1024];
    std::snprintf(child_copy, sizeof(child_copy), "%s", child);
    if (svc_file->write_all(mod_ctx, child_copy, png.data(), png.size()) != MOD_OK) {
        fail("Capture failed: write error.");
        return;
    }
    if (for_job) {
        record_job_tile();
        return;
    }
    mods::log::info("lumen: saved {}x{} capture ({} byte PNG)", g_capture.width, g_capture.height,
        png.size());
    char body[256];
    std::snprintf(body, sizeof(body), "Saved %ux%u capture.", g_capture.width, g_capture.height);
    push_photo_toast("Lumen Photo Mode", body);
}

// ---------------------------------------------------------------- gfx stage hooks

void on_scene_begin(ModContext*, const GfxStageContext* stage, void*) {
    CallbackShield shield {"scene-begin hook"};
    LUMEN_SHIELD_TRY
        g_camera_valid = stage->game_view &&
                         svc_camera->get_camera(mod_ctx, stage->game_view, &g_camera) == MOD_OK;
    LUMEN_SHIELD_CATCH(shield)
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    CallbackShield shield {"frame-before-HUD hook"};
    LUMEN_SHIELD_TRY
    // Effects held off for the job: re-assert the pins once per frame so a
    // game-side write can never leak into a tile.
    if (g_job.active)
        strip_repin();
    // Pre-warm the capture machinery on the first quiet photo-mode frame:
    // compiling the compute pipeline and allocating the ~25 MB staging pair
    // happen at a calm moment instead of inside the first capture attempt.
    // Measured 2026-11-26: a JOB capture as the first capture of a session,
    // while an area was still streaming, coincided with EXCEPTION_ACCESS_
    // VIOLATION inside amdxc64.dll (the AMD driver's D3D12 path) three
    // times; a plain capture or a settled scene never did. Cheap insurance.
    static bool s_prewarm_done = false;
    if (g_photo.active && !s_prewarm_done) {
        GfxDeviceInfo pw = GFX_DEVICE_INFO_INIT;
        GfxResolveDesc rd = GFX_RESOLVE_DESC_INIT;
        rd.color = true;
        GfxResolvedTargets rt = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->get_device_info(mod_ctx, &pw) == MOD_OK && pw.device &&
            svc_gfx->resolve_pass(mod_ctx, &rd, &rt) == MOD_OK && rt.width && rt.height) {
            g_gfx_stage = "prewarm";
            ensure_pipeline(pw.device);
            if (g_capture.pipeline) {
                ensure_capture_buffers(pw.device, rt.width * rt.height * 4u);
                s_prewarm_done = true;
                mods::log::info("lumen: capture machinery pre-warmed ({}x{})", rt.width,
                    rt.height);
            }
            g_gfx_stage = "idle";
        }
    }
    // Gigapixel job kickoff: one probe of the scene color size before aiming
    // tiles (the master grid is defined in viewport-sized cells).
    if (g_job.active && g_job.build_pending) {
        GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
        resolveDesc.color = true;
        GfxResolvedTargets targets = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &targets) == MOD_OK && targets.width &&
            targets.height) {
            // Views from resolve_pass are borrowed for this callback only.
            // Stability: the size must read the same on two consecutive
            // probes. The present target can flicker during menu/HUD
            // transitions, and freezing the grid on a transient size would
            // mismatch every tile.
            if (targets.width == g_job.probe_w && targets.height == g_job.probe_h) {
                g_job.color_format = targets.color_format;
                build_job_tiles(targets.width, targets.height);
            } else {
                g_job.probe_w = targets.width;
                g_job.probe_h = targets.height;
                mods::log::info("lumen: probe frame {} reads {}x{}", g_job.probe_frames,
                    targets.width, targets.height);
            }
        } else if (++g_job.probe_frames > 300) {
            abort_job(false);
            push_photo_toast("Lumen Photo Mode",
                "Capture job canceled: viewport size never became available.", "error");
        }
        return;
    }
    if (g_capture.phase != CAPTURE_WAIT_SETTLE || g_capture.settle_left > 0)
        return;
    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    if (svc_gfx->get_device_info(mod_ctx, &info) == MOD_OK && info.device)
        issue_capture(info.device);
    LUMEN_SHIELD_CATCH(shield)
    if (shield.fired && (g_capture.phase != CAPTURE_NONE || g_job.active)) {
        // A throw mid-capture leaves the phase machine spinning: the same
        // failure will repeat every frame. After 20 strikes, cancel the
        // capture (and its job) instead of livelocking the frame hook.
        static int s_throw_streak = 0;
        if (++s_throw_streak >= 20) {
            s_throw_streak = 0;
            fail_capture("Capture failed: the graphics layer kept rejecting the "
                         "readback (see the mod log for the error).");
        }
    }
}

// ---------------------------------------------------------------- game hooks

HookAction on_stage_change_pre(ModContext*, void*, void*, void*) {
    if (g_photo.active)
        exit_photo_mode();
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------- ui panel

void on_toggle_pressed(ModContext*, void*) {
    if (g_photo.active)
        exit_photo_mode();
    else
        enter_photo_mode();
}

void on_capture_pressed(ModContext*, void*) {
    request_capture();
}

void on_gigapixel_pressed(ModContext*, void*) {
    start_gigapixel();
}

void on_360_pressed(ModContext*, void*) {
    start_360();
}

void on_stereo_pressed(ModContext*, void*) {
    start_stereo();
}

void on_folder_picked(ModContext*, ModResult status, const char* const* locations, uint32_t count,
    const char* error, void*) {
    if (status != MOD_OK || count == 0 || !locations || !locations[0]) {
        if (status != MOD_UNAVAILABLE)
            mods::log::warn("lumen: folder pick failed: {}", error ? error : "unknown");
        return;
    }
    std::snprintf(g_folder_location, sizeof(g_folder_location), "%s", locations[0]);
    if (g_var_capture_folder)
        svc_config->set_string(mod_ctx, g_var_capture_folder, g_folder_location);
    push_photo_toast("Lumen Photo Mode", "Capture folder set.", "info");
}

void on_folder_pressed(ModContext*, void*) {
    FilePickOptions options = FILE_PICK_OPTIONS_INIT;
    options.default_location = g_folder_location[0] ? g_folder_location : nullptr;
    if (svc_file->pick_folder(mod_ctx, &options, on_folder_picked, nullptr) != MOD_OK)
        push_photo_toast("Lumen Photo Mode", "Could not open the folder picker.", "warning");
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Photo session");
    svc_ui->pane_add_text(mod_ctx, panel, status_label(), &g_status_text);

    // Say the controls out loud: players lose the camera in photo mode because
    // nothing tells them F1 gives it back.
    for (const char* line : kPanelControlLines)
        svc_ui->pane_add_text(mod_ctx, panel, line, nullptr);

    UiControlDesc help = UI_CONTROL_DESC_INIT;
    help.kind = UI_CONTROL_BUTTON;
    help.label = "Show the controls window";
    help.help_rml = "Reopens the first-run controls window.";
    help.on_pressed = on_show_controls_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &help, nullptr);

    UiControlDesc toggle = UI_CONTROL_DESC_INIT;
    toggle.kind = UI_CONTROL_BUTTON;
    toggle.label = g_photo.active ? "Return to the game (F1)" : "Enter photo mode (F1)";
    toggle.help_rml = "Freezes the scene and hands the camera to Lumen. "
                      "WASD/QE fly, arrow keys look, Shift boosts, PgUp/PgDn zoom. "
                      "F1 again gives the game back.";
    toggle.on_pressed = on_toggle_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &toggle, &g_toggle_button);

    UiControlDesc capture = UI_CONTROL_DESC_INIT;
    capture.kind = UI_CONTROL_BUTTON;
    capture.label = "Capture 1x screenshot";
    capture.help_rml = "HUD-free PNG at native window resolution.";
    capture.on_pressed = on_capture_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &capture, nullptr);

    UiControlDesc gigapixel = UI_CONTROL_DESC_INIT;
    gigapixel.kind = UI_CONTROL_BUTTON;
    gigapixel.label = "Capture gigapixel (multi-tile)";
    gigapixel.help_rml = "Renders the scene many times at the same eye, one tile per "
                         "sub-frustum, then stitch offline: lumen stitch <manifest.json>.";
    gigapixel.on_pressed = on_gigapixel_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &gigapixel, nullptr);

    UiControlDesc pano = UI_CONTROL_DESC_INIT;
    pano.kind = UI_CONTROL_BUTTON;
    pano.label = "Capture 360 panorama (six faces)";
    pano.help_rml = "Six axis-aligned faces at the frozen eye; stitch offline into an "
                    "equirectangular panorama: lumen stitch <manifest.json>.";
    pano.on_pressed = on_360_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &pano, nullptr);

    UiControlDesc stereo = UI_CONTROL_DESC_INIT;
    stereo.kind = UI_CONTROL_BUTTON;
    stereo.label = "Capture stereo trio (left / center / right)";
    stereo.help_rml = "Three views at the frozen eye with shifted eyes; stitched into one "
                      "side-by-side image: lumen stitch <manifest.json>.";
    stereo.on_pressed = on_stereo_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &stereo, nullptr);

    UiControlDesc folder = UI_CONTROL_DESC_INIT;
    folder.kind = UI_CONTROL_BUTTON;
    folder.label = "Choose capture folder...";
    folder.on_pressed = on_folder_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &folder, nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Capture settings");

    UiControlDesc scale = UI_CONTROL_DESC_INIT;
    scale.kind = UI_CONTROL_NUMBER;
    scale.label = "Gigapixel tiles per side";
    scale.help_rml = "Super resolution multiplier (2-33). Final image is this many times "
                     "the window resolution in each direction; above 16 is the opt-in "
                     "slow path.";
    scale.binding = UI_BINDING_CONFIG_VAR;
    scale.config_var = g_var_capture_scale;
    scale.min = 2;
    scale.max = 33;
    scale.suffix = "x";
    svc_ui->pane_add_control(mod_ctx, panel, &scale, nullptr);

    UiControlDesc settle = UI_CONTROL_DESC_INIT;
    settle.kind = UI_CONTROL_NUMBER;
    settle.label = "Settle frames per shot";
    settle.help_rml = "Frames to present before snapshotting so temporal effects converge.";
    settle.binding = UI_BINDING_CONFIG_VAR;
    settle.config_var = g_var_settle_frames;
    settle.min = 0;
    settle.max = 10;
    svc_ui->pane_add_control(mod_ctx, panel, &settle, nullptr);

    UiControlDesc move_speed = UI_CONTROL_DESC_INIT;
    move_speed.kind = UI_CONTROL_NUMBER;
    move_speed.label = "Flight speed";
    move_speed.binding = UI_BINDING_CONFIG_VAR;
    move_speed.config_var = g_var_move_speed;
    move_speed.min = 1;
    move_speed.max = 2000;
    move_speed.suffix = " u/s";
    svc_ui->pane_add_control(mod_ctx, panel, &move_speed, nullptr);

    UiControlDesc look_speed = UI_CONTROL_DESC_INIT;
    look_speed.kind = UI_CONTROL_NUMBER;
    look_speed.label = "Look speed";
    look_speed.binding = UI_BINDING_CONFIG_VAR;
    look_speed.config_var = g_var_look_speed;
    look_speed.min = 5;
    look_speed.max = 180;
    look_speed.suffix = " deg/s";
    svc_ui->pane_add_control(mod_ctx, panel, &look_speed, nullptr);

    UiControlDesc fov = UI_CONTROL_DESC_INIT;
    fov.kind = UI_CONTROL_NUMBER;
    fov.label = "Photo FOV";
    fov.help_rml = "Vertical field of view for the photo camera. 90 deg tiles seamlessly for "
                   "gigapixel panoramas.";
    fov.binding = UI_BINDING_CONFIG_VAR;
    fov.config_var = g_var_photo_fov;
    fov.min = 5;
    fov.max = 170;
    fov.suffix = " deg";
    svc_ui->pane_add_control(mod_ctx, panel, &fov, nullptr);

    UiControlDesc ipd = UI_CONTROL_DESC_INIT;
    ipd.kind = UI_CONTROL_NUMBER;
    ipd.label = "Stereo eye separation";
    ipd.help_rml = "Distance between the left and right capture eyes for stereo jobs, in "
                   "hundredths of a world unit (100 = one unit). Larger is stronger 3D but "
                   "harder to fuse.";
    ipd.binding = UI_BINDING_CONFIG_VAR;
    ipd.config_var = g_var_stereo_ipd;
    ipd.min = 1;
    ipd.max = 2000;
    ipd.suffix = " x0.01u";
    svc_ui->pane_add_control(mod_ctx, panel, &ipd, nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Gigapixel consistency");
    svc_ui->pane_add_text(mod_ctx, panel,
        "Off by default: captures keep the look of the game (bloom, haze and fade stay on). "
        "Turn one on if the seams between tiles bother you - motion blur stays stripped "
        "because it would ghost the previous tile into the next.", nullptr);
    struct StripToggleDesc {
        const char* label;
        const char* help;
        ConfigVarHandle var;
    };
    const StripToggleDesc stripToggles[] = {
        {"Strip cloud-shadow haze",
            "Skips the moya haze draw while a multi-tile job runs. It is drawn in screen space, "
            "so its exact pattern differs between tiles.", g_var_strip_haze},
        {"Strip bloom",
            "Turns bloom off during multi-tile jobs. It composites over the whole viewport, so "
            "it can differ across tile borders - but bloom adds a lot of the scene's light, so "
            "stripping it makes captures darker and flatter than the game.",
            g_var_strip_bloom},
        {"Strip fade",
            "Forces the screen fade to zero during multi-tile jobs, so a fade that changes "
            "between tiles cannot show up as a brightness step.", g_var_strip_fade},
        {"Strip motion blur (recommended)",
            "Turns motion blur off during multi-tile jobs: it is an EFB trail, so without this "
            "tile N ghosts the image of tile N-1. On by default.", g_var_strip_blur},
    };
    for (const StripToggleDesc& t : stripToggles) {
        UiControlDesc ctl = UI_CONTROL_DESC_INIT;
        ctl.kind = UI_CONTROL_TOGGLE;
        ctl.label = t.label;
        ctl.help_rml = t.help;
        ctl.binding = UI_BINDING_CONFIG_VAR;
        ctl.config_var = t.var;
        svc_ui->pane_add_control(mod_ctx, panel, &ctl, nullptr);
    }

    {
        UiControlDesc ctl = UI_CONTROL_DESC_INIT;
        ctl.kind = UI_CONTROL_TOGGLE;
        ctl.label = "Companion-mod hint (Twilit Essentials)";
        ctl.help_rml = "Once per game session, when a multi-tile job starts and "
                       "Twilit Essentials is installed, Lumen reminds you that its Wind, "
                       "3D Grass, volumetric Clouds and Color & Lens effects animate on "
                       "every render frame and keep frame history - actor freeze cannot "
                       "stop them, so their toggles must be turned off in the Essentials "
                       "Graphics menu for perfectly seamless gigapixel/360 images. "
                       "Turn this off if the reminder bothers you.";
        ctl.binding = UI_BINDING_CONFIG_VAR;
        ctl.config_var = g_var_essentials_hint;
        svc_ui->pane_add_control(mod_ctx, panel, &ctl, nullptr);
    }

    {
        UiControlDesc ctl = UI_CONTROL_DESC_INIT;
        ctl.kind = UI_CONTROL_TOGGLE;
        ctl.label = "Swap red and blue in captures";
        ctl.help_rml = "Only for graphics backends that read the scene buffer's red and blue "
                       "reversed. Off is correct on Dusklight 2.0.3: captures were measured to "
                       "match the game's colours exactly with this off.";
        ctl.binding = UI_BINDING_CONFIG_VAR;
        ctl.config_var = g_var_swap_rb;
        svc_ui->pane_add_control(mod_ctx, panel, &ctl, nullptr);
    }

    {
        UiControlDesc ctl = UI_CONTROL_DESC_INIT;
        ctl.kind = UI_CONTROL_TOGGLE;
        ctl.label = "Freeze the world in photo mode";
        ctl.help_rml = "Sets the game's own global pause (the same flag the 'freeze' console "
                       "command uses), so actors, enemies, NPCs and their animations stand "
                       "still while you frame a shot. Release restores the previous state.";
        ctl.binding = UI_BINDING_CONFIG_VAR;
        ctl.config_var = g_var_freeze_world;
        svc_ui->pane_add_control(mod_ctx, panel, &ctl, nullptr);
    }

    {
        UiControlDesc ctl = UI_CONTROL_DESC_INIT;
        ctl.kind = UI_CONTROL_TOGGLE;
        ctl.label = "Auto-stitch finished jobs";
        ctl.help_rml = "Composites gigapixel / 360 / stereo jobs into a tiled BigTIFF "
                       "on a background thread as soon as the last part is saved.";
        ctl.binding = UI_BINDING_CONFIG_VAR;
        ctl.config_var = g_var_auto_stitch;
        svc_ui->pane_add_control(mod_ctx, panel, &ctl, nullptr);
    }

    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_status_text)
        svc_ui->elem_set_text(mod_ctx, g_status_text, status_label());
    if (g_toggle_button)
        svc_ui->control_set_label(mod_ctx, g_toggle_button,
            g_photo.active ? "Return to the game (F1)" : "Enter photo mode (F1)");
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_var_capture_scale = register_var_typed("captureScale", CONFIG_VAR_INT, 4, 0);
    g_var_settle_frames = register_var_typed("settleFrames", CONFIG_VAR_INT, 4, 0);
    // NUMBER controls bind INT vars only (the UI service type-checks the
    // binding: CONFIG_VAR_FLOAT is rejected with "type does not match the
    // control kind"), so the flight/look/FOV settings are ints. stereoIpd is
    // hundredths of a world unit to keep sub-unit eye separation adjustable.
    g_var_move_speed = register_var_typed("moveSpeed", CONFIG_VAR_INT, 100, 0);
    g_var_look_speed = register_var_typed("lookSpeed", CONFIG_VAR_INT, 45, 0);
    g_var_stereo_ipd = register_var_typed("stereoIpd", CONFIG_VAR_INT, 100, 0);
    g_var_photo_fov = register_var_typed("photoFov", CONFIG_VAR_INT, 90, 0);
    g_var_capture_folder = register_var_typed("captureFolder", CONFIG_VAR_STRING, 0, 0);
    cfg_string(g_var_capture_folder, g_folder_location, sizeof(g_folder_location));
    // Consistency defaults: only the EFB-trail motion blur is stripped by
    // default. Bloom / haze / fade are the passes the player sees, so taking
    // them out changes the picture (darker, flatter) - opt-in for seam purity.
    g_var_strip_haze = register_var_typed("stripHaze", CONFIG_VAR_BOOL, 0, 0);
    g_var_strip_bloom = register_var_typed("stripBloom", CONFIG_VAR_BOOL, 0, 0);
    g_var_strip_fade = register_var_typed("stripFade", CONFIG_VAR_BOOL, 0, 0);
    g_var_strip_blur = register_var_typed("stripBlur", CONFIG_VAR_BOOL, 1, 0);
    g_var_essentials_hint = register_var_typed("essentialsHint", CONFIG_VAR_BOOL, 1, 0);
    g_var_auto_stitch = register_var_typed("autoStitch", CONFIG_VAR_BOOL, 1, 0);
    g_var_freeze_world = register_var_typed("freezeWorld", CONFIG_VAR_BOOL, 1, 0);
    g_var_swap_rb = register_var_typed("swapRB", CONFIG_VAR_BOOL, 0, 0);
    g_var_seen_controls = register_var_typed("seenControls", CONFIG_VAR_BOOL, 0, 0);
    g_pins[0].toggle = g_var_strip_blur;
    g_pins[1].toggle = g_var_strip_fade;
    g_pins[2].toggle = g_var_strip_bloom;

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "lumen_readback";
    computeDesc.callback = on_readback_compute;
    bool gfx_ready =
        svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_compute_type) == MOD_OK;
    if (!gfx_ready)
        mods::log::warn("lumen: could not register readback compute type");

    GfxStageHookDesc sceneBeginDesc = GFX_STAGE_HOOK_DESC_INIT;
    sceneBeginDesc.callback = on_scene_begin;
    ModResult r = svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_SCENE_BEGIN, &sceneBeginDesc,
                                               &g_hook_scene_begin);
    if (r != MOD_OK) {
        mods::log::warn("lumen: scene-begin stage hook failed ({})", static_cast<int>(r));
        gfx_ready = false;
    }
    GfxStageHookDesc frameDesc = GFX_STAGE_HOOK_DESC_INIT;
    frameDesc.callback = on_frame_before_hud;
    r = svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &frameDesc, &g_hook_frame);
    if (r != MOD_OK) {
        mods::log::warn("lumen: frame stage hook failed ({})", static_cast<int>(r));
        gfx_ready = false;
    }
    // Older GfxService minors without the resolve/readback entry points fail
    // here; the mod stays loaded but photo mode is gated off (feature floor).
    g_feat_capture = gfx_ready;
    if (!g_feat_capture)
        mods::log::warn("lumen: capture path unavailable - degraded, photo mode stays off");

    g_feat_autoexit = mods::hook::add_pre<StageChangeScene>(on_stage_change_pre) == MOD_OK;
    if (!g_feat_autoexit)
        mods::log::warn("lumen: stage change hook unavailable; auto-exit disabled");
    // Gigapixel consistency: haze draw skip + post-effect state pinning. Both
    // are optional; the job still runs (with possible tile-to-tile seams) when
    // a symbol is missing from this build's manifest.
    if (mods::hook::add_pre<CloudShadow>(on_cloud_shadow_pre) == MOD_OK)
        g_haze_hooked = true;
    else
        mods::log::warn("lumen: cloud-shadow hook unavailable; haze strip disabled");
    resolve_effect_symbols();
    g_feat_strip = g_haze_hooked;
    for (const DataPin& pin : g_pins)
        g_feat_strip = g_feat_strip || pin.addr != nullptr;

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        mods::set_error(error, MOD_ERROR, "failed to register mods panel");
        return MOD_ERROR;
    }

    // No HostService import on purpose: every import raises the service
    // minor floor the loader will accept, and the host is only interesting
    // for support chatter. The feature line below says what matters.
    mods::log::info(
        "lumen: features: capture {}, world freeze {}, auto-exit {}, consistency {}",
        g_feat_capture ? "yes" : "NO", g_feat_world_freeze ? "yes" : "no",
        g_feat_autoexit ? "yes" : "no", g_feat_strip ? "yes" : "no");
    mods::log::info("Lumen Photo Mode {} loaded (M6: photo + world freeze + gigapixel + 360 + "
                    "stereo + in-mod auto-stitch)",
        "0.1.0");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    (void)error;
    poll_hotkeys();
    // Real frame delta: mod_update runs once per render frame, so a fixed
    // 1/60 makes flight half speed at 120 fps and double at 30. Clamped so
    // alt-tab hitches or a shader-compile stall can't teleport the camera.
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - s_last).count();
    s_last = now;
    if (dt < 0.0005f)
        dt = 0.0005f;
    if (dt > 0.1f)
        dt = 0.1f;
    apply_flight_input(dt);

    // Deferred camera handoff: exit_photo_mode keeps the operator registered
    // until the saved game pose has been written back (photo_operate clears
    // have_saved when the restore window ends); unregister here, on the game
    // thread, not from inside the camera callback.
    if (g_operator && !g_photo.active && !g_photo.have_saved) {
        svc_camera->unregister_camera_operator(mod_ctx, g_operator);
        g_operator = 0;
    }

    // Background-stitch completions (worker thread -> game thread).
    if (!g_stitch_running) {
        std::string done;
        bool ok = false;
        {
            std::lock_guard lock(g_stitch_mtx);
            if (!g_stitch_done.empty()) {
                done.swap(g_stitch_done);
                ok = g_stitch_done_ok;
            }
        }
        if (!done.empty()) {
            if (ok) {
                char body[256];
                std::snprintf(body, sizeof(body), "Stitched: %s", done.c_str());
                push_photo_toast("Lumen Photo Mode", body);
                mods::log::info("lumen: auto-stitch finished -> {}", done);
            } else {
                push_photo_toast("Lumen Photo Mode", done.c_str(), "error");
                mods::log::warn("lumen: auto-stitch failed: {}", done);
            }
        }
    }

    // In-mode cheat sheet: restate the exit route while photo mode idles. The
    // world stands still and nothing else on screen says which key gives the
    // game back, so the reminder repeats. The heartbeat line is diagnostic:
    // if it stops mid-session, the game stopped calling mod_update.
    if (g_photo.active && !g_job.active && !g_stitch_running) {
        static uint32_t s_beat = 0;
        if (std::chrono::steady_clock::now() >= g_photo_remind_at) {
            g_photo_remind_at = std::chrono::steady_clock::now() + std::chrono::seconds(12);
            push_photo_toast("Lumen Photo Mode",
                "Photo mode - press F1 to return to the game. F7 capture, F12 gigapixel, "
                "Shift+F12 360, Shift+F7 stereo; WASD + arrows fly the camera.",
                "info", 5000);
        }
        if (++s_beat % 900 == 0)
            mods::log::info("lumen: photo mode alive ({} updates)", s_beat);
    }

    // Stall watchdog: a capture whose map never completes (lost device,
    // wedged queue) must not pin photo mode or a job forever. Generous at
    // any plausible frame rate; PNG encode is synchronous and far faster.
    if (g_capture.phase != CAPTURE_NONE && ++g_capture.stall_frames > 900)
        fail_capture("Capture stalled: the GPU readback never completed.");

    if (g_capture.phase == CAPTURE_WAIT_SETTLE && g_capture.settle_left > 0)
        g_capture.settle_left--;
    // The readback copy is submitted with the frame that recorded it. Map a frame
    // later so the mapping queues behind that submission.
    if (g_capture.phase == CAPTURE_WAIT_COPY) {
        if (g_capture.staging) {
            // The callback only records failures; success is observed by
            // polling the map state, which the game's per-frame device
            // activity drives to Mapped.
            WGPUBufferMapCallbackInfo map_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            map_info.mode = WGPUCallbackMode_AllowProcessEvents;
            map_info.callback = on_map_complete;
            wgpuBufferMapAsync(g_capture.staging, WGPUMapMode_Read, 0,
                static_cast<uint64_t>(g_capture.width) * g_capture.height * 4ull,
                map_info);
            g_capture.phase = CAPTURE_WAIT_MAP;
        } else {
            g_capture.phase = CAPTURE_NONE;
        }
    }
    if (g_capture.map_failed) {
        g_capture.map_failed = false;
        fail_capture("GPU readback map failed (device or buffer error).");
    }
    if (g_capture.phase == CAPTURE_WAIT_MAP && g_capture.staging &&
        wgpuBufferGetMapState(g_capture.staging) == WGPUBufferMapState_Mapped) {
        finish_capture_file();
    }
    // Drive the gigapixel job: start the next tile once the previous one is
    // fully written.
    if (g_job.active && !g_job.build_pending && g_capture.phase == CAPTURE_NONE &&
        g_job.idx < g_job.tiles.size()) {
        advance_job();
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    abort_job(false);
    g_photo.active = false;
    g_photo.frozen = false;
    // Never leave the game parked in its own global pause because the mod was
    // disabled or reloaded mid-shot.
    world_freeze_end();
    if (g_operator) {
        svc_camera->unregister_camera_operator(mod_ctx, g_operator);
        g_operator = 0;
    }
    // The stitch worker runs code from this module: an unload/reload while it
    // is stitching would unmap running code. Wait for it to finish.
    if (g_stitch_worker.joinable())
        g_stitch_worker.join();
    if (g_hook_scene_begin) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_hook_scene_begin);
        g_hook_scene_begin = 0;
    }
    if (g_hook_frame) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_hook_frame);
        g_hook_frame = 0;
    }
    if (g_compute_type) {
        svc_gfx->unregister_compute_type(mod_ctx, g_compute_type);
        g_compute_type = 0;
    }
    reset_capture_buffers();
    if (g_capture.pipeline) {
        wgpuComputePipelineRelease(g_capture.pipeline);
        g_capture.pipeline = nullptr;
    }
    (void)error;
    return MOD_OK;
}

}  // extern "C"
