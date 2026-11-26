// Lumen Photo Mode - Dusklight integration (Ansel-style photography for Dusklight).
//
// M1 scope: photo sessions with a free camera (CameraService operator), time freeze
// (HookService post-hook on dusk::game_clock::advance zeroing numSimTicks), HUD-free
// native-resolution PNG capture (GfxService resolve_pass at FRAME_BEFORE_HUD + WebGPU
// compute readback), hotkeys, and capture folder persistence.
//
// Controls (Windows; panel buttons cover every action on other platforms):
//   F9  enter / exit photo mode      F10 capture (1x)
//   WASD move, Q/E down/up, arrows look, Shift boost, PgUp/PgDn field of view.
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
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
// gracefully: no freeze / no auto-exit rather than a dead mod.
DEFINE_HOOK_SYMBOL("dusk::game_clock::advance", void*(), GameClockAdvance);
DEFINE_HOOK_SYMBOL("dStage_changeScene", int(int, float, unsigned, signed char, short, int),
    StageChangeScene);

namespace {

// Layout mirror of dusk::game_clock::FrameTiming (src/dusk/game_clock.h).
struct FrameTimingMirror {
    float dt;
    bool interpolating;
    bool separatePresentation;
    int numSimTicks;
    uint64_t presentationEpoch;
};

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
};
PhotoState g_photo;

struct CaptureState {
    CapturePhase phase = CAPTURE_NONE;
    int settle_left = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Mod-owned per-capture readback objects.
    WGPUBuffer staging = nullptr;  // MAP_READ | COPY_DST
    WGPUBuffer storage = nullptr;  // STORAGE | COPY_SRC (compute output)
    uint32_t uniform_offset = 0;   // dims slot inside the frame uniform stream
    // Persistent pipeline.
    WGPUComputePipeline pipeline = nullptr;
};
CaptureState g_capture;

// Latest world camera, refreshed at GFX_STAGE_SCENE_BEGIN (world-camera stage).
CameraInfo g_camera = CAMERA_INFO_INIT;
bool g_camera_valid = false;

// Config vars.
ConfigVarHandle g_var_capture_scale = 0;
ConfigVarHandle g_var_settle_frames = 0;
ConfigVarHandle g_var_move_speed = 0;
ConfigVarHandle g_var_look_speed = 0;
ConfigVarHandle g_var_photo_fov = 0;
ConfigVarHandle g_var_capture_folder = 0;

char g_folder_location[1024] = {0}; // persisted opaque FileService location

// ---------------------------------------------------------------- config helpers

ConfigVarHandle register_var_typed(const char* name, ConfigVarType type, int64_t i, double f) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
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

double cfg_float(ConfigVarHandle var, double def) {
    double v = def;
    if (var && svc_config->get_float(mod_ctx, var, &v) != MOD_OK)
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

// ---------------------------------------------------------------- ui helpers

UiElementHandle g_status_text = 0;

const char* status_label() {
    if (!g_photo.active)
        return "photo mode: off (F9)";
    return g_photo.frozen ? "photo mode: ON, time frozen"
                          : "photo mode: ON, time running";
}

void push_photo_toast(const char* title, const char* body, const char* type = "success") {
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = type;
    toast.title_rml = title;
    toast.body_rml = body;
    toast.duration_ms = 3000;
    svc_ui->push_toast(mod_ctx, &toast);
}

// ---------------------------------------------------------------- camera operator

bool photo_operate(ModContext*, CameraOperatorState* state, void*) {
    if (!g_photo.active)
        return false;
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
    }
    state->eye[0] = g_photo.eye[0];
    state->eye[1] = g_photo.eye[1];
    state->eye[2] = g_photo.eye[2];
    const float cp = std::cosf(g_photo.pitch);
    state->center[0] = g_photo.eye[0] + cp * std::sinf(g_photo.yaw);
    state->center[1] = g_photo.eye[1] + std::sinf(g_photo.pitch);
    state->center[2] = g_photo.eye[2] + cp * std::cosf(g_photo.yaw);
    state->fovy = g_photo.fovy;
    state->bank_degrees = 0;
    return true;
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
}

// ---------------------------------------------------------------- photo session

bool folder_ok() {
    return g_folder_location[0] != '\0' &&
           svc_file->check(mod_ctx, g_folder_location) == MOD_OK;
}

ModResult request_capture() {
    if (!g_photo.active) {
        push_photo_toast("Lumen Photo Mode", "Enter photo mode first (F9).", "warning");
        return MOD_ERROR;
    }
    if (g_capture.phase != CAPTURE_NONE)
        return MOD_OK; // one shot at a time in M1
    g_capture.settle_left = static_cast<int>(cfg_int(g_var_settle_frames, 2));
    g_capture.phase = CAPTURE_WAIT_SETTLE;
    return MOD_OK;
}

void enter_photo_mode() {
    if (g_photo.active)
        return;
    g_photo.active = true;
    g_photo.seeded = false;
    g_photo.fovy = static_cast<float>(cfg_float(g_var_photo_fov, 90.0));

    CameraOperatorDesc desc = CAMERA_OPERATOR_DESC_INIT;
    desc.debug_name = "lumen_photo";
    desc.priority = 1000;
    desc.operate = photo_operate;
    if (svc_camera->register_camera_operator(mod_ctx, &desc, &g_operator) != MOD_OK) {
        g_operator = 0;
        mods::log::warn("lumen: camera operator registration failed");
    }

    // Time freeze: the advance() post-hook zeroes numSimTicks while frozen. Only
    // engage if the hook resolved at load.
    g_photo.frozen = GameClockAdvance::g_orig != nullptr;

    push_photo_toast("Lumen Photo Mode",
        g_photo.frozen ? "Photo mode on - time frozen. WASD to fly, F10 to capture."
                       : "Photo mode on (time freeze unavailable). F10 to capture.");
    mods::log::info("lumen: photo mode on (frozen={})", g_photo.frozen);
}

void exit_photo_mode() {
    if (!g_photo.active)
        return;
    g_photo.active = false;
    g_photo.frozen = false;
    if (g_operator) {
        svc_camera->unregister_camera_operator(mod_ctx, g_operator);
        g_operator = 0;
    }
    if (g_capture.phase != CAPTURE_NONE) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
    }
    push_photo_toast("Lumen Photo Mode", "Photo mode off.");
    mods::log::info("lumen: photo mode off");
}

// ---------------------------------------------------------------- input (Windows)

#ifdef _WIN32
bool key_held(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool g_prev_f9 = false;
bool g_prev_f10 = false;

void poll_hotkeys() {
    const bool f9 = key_held(VK_F9);
    if (f9 && !g_prev_f9) {
        if (g_photo.active)
            exit_photo_mode();
        else
            enter_photo_mode();
    }
    g_prev_f9 = f9;

    const bool f10 = key_held(VK_F10);
    if (f10 && !g_prev_f10)
        request_capture();
    g_prev_f10 = f10;
}

void apply_flight_input(float dt) {
    if (!g_photo.active)
        return;
    float speed = static_cast<float>(cfg_float(g_var_move_speed, 20.0));
    if (key_held(VK_SHIFT))
        speed *= 4.0f;
    const float look = static_cast<float>(cfg_float(g_var_look_speed, 45.0)) * kDegToRad;

    if (key_held(VK_LEFT)) g_photo.yaw += look * dt;
    if (key_held(VK_RIGHT)) g_photo.yaw -= look * dt;
    if (key_held(VK_UP)) g_photo.pitch += look * dt;
    if (key_held(VK_DOWN)) g_photo.pitch -= look * dt;
    if (g_photo.pitch > 1.55f) g_photo.pitch = 1.55f;
    if (g_photo.pitch < -1.55f) g_photo.pitch = -1.55f;
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
struct P { w:u32, h:u32, sp0:u32, sp1:u32 };
@group(0) @binding(2) var<uniform> p : P;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= p.w || gid.y >= p.h) {
        return;
    }
    let c = textureLoad(src, vec2<i32>(i32(gid.x), i32(gid.y)), 0);
    let q = vec4<u32>(vec4<i32>(clamp(c, vec4<f32>(0.0), vec4<f32>(1.0)) * vec4<f32>(255.0) + vec4<f32>(0.5)));
    dst[gid.y * p.w + gid.x] = q.x | (q.y << 8u) | (q.z << 16u) | (q.w << 24u);
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
    }
    wgpuCommandEncoderCopyBufferToBuffer(compute->encoder, p.storage, 0, p.staging, 0, p.bytes);
    wgpuBindGroupRelease(bindGroup);
}

GfxComputeTypeHandle g_compute_type = 0;

void issue_capture(WGPUDevice device) {
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    GfxResolvedTargets targets = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &targets) != MOD_OK || !targets.color) {
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: no scene color snapshot.", "error");
        return;
    }
    const uint32_t w = targets.width;
    const uint32_t h = targets.height;
    const uint32_t bytes = w * h * 4u;
    ensure_pipeline(device);
    if (!g_capture.pipeline) {
        g_capture.phase = CAPTURE_NONE;
        return;
    }

    reset_capture_buffers();
    {
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = {"lumen_staging", WGPU_STRLEN};
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bd.size = bytes;
        g_capture.staging = wgpuDeviceCreateBuffer(device, &bd);
        bd.label = {"lumen_storage", WGPU_STRLEN};
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
        g_capture.storage = wgpuDeviceCreateBuffer(device, &bd);
    }
    if (!g_capture.staging || !g_capture.storage) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: buffer allocation failed.", "error");
        return;
    }
    // Dims ride the frame's shared uniform stream; binding 2 resolves to that
    // buffer's pushed slot inside the compute context.
    const uint32_t dims[4] = {w, h, 0, 0};
    GfxRange uniformRange = {};
    if (svc_gfx->push_uniform(mod_ctx, dims, sizeof(dims), &uniformRange) != MOD_OK) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: uniform stream rejected.", "error");
        return;
    }

    ReadbackPayload payload{targets.color, g_capture.storage, g_capture.staging, bytes,
        uniformRange.offset, w, h};
    if (svc_gfx->push_compute(mod_ctx, g_compute_type, &payload, sizeof(payload)) != MOD_OK) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: compute task rejected.", "error");
        return;
    }
    g_capture.width = w;
    g_capture.height = h;
    g_capture.phase = CAPTURE_WAIT_COPY;
}

// ---------------------------------------------------------------- png sink

void png_write_cb(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

void finish_capture_file() {
    const uint64_t bytes = static_cast<uint64_t>(g_capture.width) * g_capture.height * 4ull;
    const void* mapped = wgpuBufferGetConstMappedRange(g_capture.staging, 0, bytes);
    if (!mapped) {
        reset_capture_buffers();
        g_capture.phase = CAPTURE_NONE;
        push_photo_toast("Lumen Photo Mode", "Capture failed: readback unavailable.", "error");
        return;
    }
    std::vector<uint8_t> png;
    const int ok = stbi_write_png_to_func(png_write_cb, &png, static_cast<int>(g_capture.width),
        static_cast<int>(g_capture.height), 4, mapped, static_cast<int>(g_capture.width * 4));
    reset_capture_buffers();
    g_capture.phase = CAPTURE_NONE;
    if (!ok || png.empty()) {
        push_photo_toast("Lumen Photo Mode", "Capture failed: PNG encode error.", "error");
        return;
    }
    if (!folder_ok()) {
        push_photo_toast("Lumen Photo Mode", "Set a capture folder in the Lumen panel first.",
            "warning");
        return;
    }
    const long long now = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    char name[128];
    std::snprintf(name, sizeof(name), "lumen-%lld-%ux%u.png", now, g_capture.width,
        g_capture.height);
    const char* child = nullptr;
    if (svc_file->create_child(mod_ctx, g_folder_location, name, &child) != MOD_OK || !child) {
        push_photo_toast("Lumen Photo Mode", "Capture failed: could not create file.", "error");
        return;
    }
    char child_copy[1024];
    std::snprintf(child_copy, sizeof(child_copy), "%s", child);
    if (svc_file->write_all(mod_ctx, child_copy, png.data(), png.size()) != MOD_OK) {
        push_photo_toast("Lumen Photo Mode", "Capture failed: write error.", "error");
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
    g_camera_valid = stage->game_view &&
                     svc_camera->get_camera(mod_ctx, stage->game_view, &g_camera) == MOD_OK;
}

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    if (g_capture.phase != CAPTURE_WAIT_SETTLE || g_capture.settle_left > 0)
        return;
    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    if (svc_gfx->get_device_info(mod_ctx, &info) == MOD_OK && info.device)
        issue_capture(info.device);
}

// ---------------------------------------------------------------- game hooks

HookAction on_stage_change_pre(ModContext*, void*, void*, void*) {
    if (g_photo.active)
        exit_photo_mode();
    return HOOK_CONTINUE;
}

void on_advance_post(ModContext*, void*, void* retval, void*) {
    if (!g_photo.active || !g_photo.frozen || !retval)
        return;
    static_cast<FrameTimingMirror*>(retval)->numSimTicks = 0;
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

    UiControlDesc toggle = UI_CONTROL_DESC_INIT;
    toggle.kind = UI_CONTROL_BUTTON;
    toggle.label = "Toggle photo mode";
    toggle.help_rml = "Freezes the scene and hands the camera to Lumen. "
                      "WASD/QE fly, arrows look, Shift boosts, PgUp/PgDn zoom.";
    toggle.on_pressed = on_toggle_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &toggle, nullptr);

    UiControlDesc capture = UI_CONTROL_DESC_INIT;
    capture.kind = UI_CONTROL_BUTTON;
    capture.label = "Capture 1x screenshot";
    capture.help_rml = "HUD-free PNG at native window resolution.";
    capture.on_pressed = on_capture_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &capture, nullptr);

    UiControlDesc folder = UI_CONTROL_DESC_INIT;
    folder.kind = UI_CONTROL_BUTTON;
    folder.label = "Choose capture folder...";
    folder.on_pressed = on_folder_pressed;
    svc_ui->pane_add_control(mod_ctx, panel, &folder, nullptr);

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
    move_speed.max = 200;
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
    g_var_capture_scale = register_var_typed("captureScale", CONFIG_VAR_INT, 4, 0);
    g_var_settle_frames = register_var_typed("settleFrames", CONFIG_VAR_INT, 2, 0);
    g_var_move_speed = register_var_typed("moveSpeed", CONFIG_VAR_FLOAT, 0, 20.0);
    g_var_look_speed = register_var_typed("lookSpeed", CONFIG_VAR_FLOAT, 0, 45.0);
    g_var_photo_fov = register_var_typed("photoFov", CONFIG_VAR_FLOAT, 0, 90.0);
    g_var_capture_folder = register_var_typed("captureFolder", CONFIG_VAR_STRING, 0, 0);
    cfg_string(g_var_capture_folder, g_folder_location, sizeof(g_folder_location));

    GfxComputeTypeDesc computeDesc = GFX_COMPUTE_TYPE_DESC_INIT;
    computeDesc.label = "lumen_readback";
    computeDesc.callback = on_readback_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &computeDesc, &g_compute_type) != MOD_OK)
        mods::log::warn("lumen: could not register readback compute type");

    GfxStageHookDesc sceneBeginDesc = GFX_STAGE_HOOK_DESC_INIT;
    sceneBeginDesc.callback = on_scene_begin;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_SCENE_BEGIN, &sceneBeginDesc, nullptr) !=
        MOD_OK)
        mods::log::warn("lumen: scene-begin stage hook failed");
    GfxStageHookDesc frameDesc = GFX_STAGE_HOOK_DESC_INIT;
    frameDesc.callback = on_frame_before_hud;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &frameDesc, nullptr) !=
        MOD_OK)
        mods::log::warn("lumen: frame stage hook failed");

    if (mods::hook::add_post<GameClockAdvance>(on_advance_post) != MOD_OK)
        mods::log::warn("lumen: game clock hook unavailable; time freeze disabled");
    if (mods::hook::add_pre<StageChangeScene>(on_stage_change_pre) != MOD_OK)
        mods::log::warn("lumen: stage change hook unavailable; auto-exit disabled");

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        mods::set_error(error, MOD_ERROR, "failed to register mods panel");
        return MOD_ERROR;
    }

    mods::log::info("Lumen Photo Mode {} loaded (M1)", "0.1.0");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    (void)error;
    poll_hotkeys();
    apply_flight_input(1.0f / 60.0f);

    if (g_capture.phase == CAPTURE_WAIT_SETTLE && g_capture.settle_left > 0)
        g_capture.settle_left--;
    // The readback copy is submitted with the frame that recorded it. Map a frame
    // later so the mapping queues behind that submission.
    if (g_capture.phase == CAPTURE_WAIT_COPY) {
        if (g_capture.staging) {
            // No callback: completion is observed by polling the map state, which
            // the game's per-frame device activity drives to Mapped.
            wgpuBufferMapAsync(g_capture.staging, WGPUMapMode_Read, 0,
                static_cast<uint64_t>(g_capture.width) * g_capture.height * 4ull,
                WGPU_BUFFER_MAP_CALLBACK_INFO_INIT);
            g_capture.phase = CAPTURE_WAIT_MAP;
        } else {
            g_capture.phase = CAPTURE_NONE;
        }
    }
    if (g_capture.phase == CAPTURE_WAIT_MAP && g_capture.staging &&
        wgpuBufferGetMapState(g_capture.staging) == WGPUBufferMapState_Mapped) {
        finish_capture_file();
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    g_photo.active = false;
    g_photo.frozen = false;
    if (g_operator) {
        svc_camera->unregister_camera_operator(mod_ctx, g_operator);
        g_operator = 0;
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
