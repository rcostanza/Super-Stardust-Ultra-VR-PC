// stardust: shadPS4 settings used by the video core, read once from STARDUST_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include "common/types.h"

double StardustWindowRefreshRate(); // stardust_gpu.cpp: exact rate of the window's display, 0 when unknown
bool StardustWaitForVblank(); // stardust_gpu.cpp: waits for the window's display to start a refresh; false when it cannot (not Windows)
extern "C" int stardust_render_4k(void); // stardust_settings.cpp: render_resolution=4k
extern "C" int stardust_render_height(void); // stardust_settings.cpp: render_resolution lines
extern "C" int stardust_width_4k(void);  // stardust_settings.cpp: the display width at 2160 lines
inline bool StardustRender4k() { return stardust_render_4k() != 0; }
extern "C" int stardust_hdr(void); // stardust_settings.cpp: settings.ini hdr
extern "C" int stardust_frame_rate(void); // stardust_settings.cpp: frame_rate (30, 60, 90, 120, 144, 180)

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("STARDUST_GPU_ID", -1)); return value; }
    // stardust: the video out size, the render_resolution's lines (stardust_render_height) at
    // the shape of stardust_width_4k (3840 at 2160 lines for 16:9).
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("STARDUST_INTERNAL_WIDTH", stardust_width_4k() * stardust_render_height() / 2160)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("STARDUST_INTERNAL_HEIGHT", stardust_render_height())); return value; }
    // stardust: Relaxed by default: without readbacks, guest code that reads GPU-written
    // vertices sees stale data; Relaxed costs no measurable frame rate.
    u32 GetReadbacksMode() { static const auto value = u32(Number("STARDUST_READBACKS", GpuReadbacksMode::Relaxed)); return value; }
    // stardust: the vblank runs at settings.ini frame_rate, read at each call (the Display
    // Settings panel changes it): the game waits for one vblank a frame. At 30 it stays 60: an
    // engine target of 30 makes the game set a flip rate of 1 (a flip every second vblank), as
    // the PS4's 30 FPS games do, so a 30 Hz vblank ran it at 15. STARDUST_VBLANK_HZ fixes it
    // (measurements).
    u32 GetVblankFrequency() {
        static const long fixed = Number("STARDUST_VBLANK_HZ", -1);
        return fixed > 0 ? u32(fixed) : u32(std::max(stardust_frame_rate(), 60));
    }
    // The vblank period. A display within 0.5 Hz of the vblank rate (a 59.95 Hz panel at the
    // default 60) sets it exactly: at 60.00 Hz the game made 0.05 frames a second more than the
    // display shows, so after ~35 s the VSync (Fifo) queue was full and the GPU thread waited on
    // the display for 4.9 s of every 5 (frames of extra latency, 33 ms frames). The display is
    // the one the window opened on. An explicit STARDUST_VBLANK_HZ is used as given.
    std::chrono::nanoseconds GetVblankPeriod() {
        static const bool snap = std::getenv("STARDUST_VBLANK_HZ") == nullptr;
        return DisplayPeriod(GetVblankFrequency(), snap);
    }
    static std::chrono::nanoseconds DisplayPeriod(double hz, bool snap) {
        static const double display = StardustWindowRefreshRate();
        return std::chrono::nanoseconds(s64(1e9 / (snap && std::abs(display - hz) < 0.5 ? display : hz) + 0.5));
    }
    bool IsCopyGpuBuffers() { static const auto value = Flag("STARDUST_COPY_GPU_BUFFERS", false); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("STARDUST_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("STARDUST_DUMP_SHADERS", false); return value; }
    // stardust: always off, the game has no HDR output (stardust_hdr).
    bool IsHdrAllowed() { static const bool value = stardust_hdr() != 0; return value; }
    bool IsNullGPU() { static const auto value = Flag("STARDUST_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("STARDUST_PIPELINE_CACHE", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("STARDUST_READBACK_LINEAR", false); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("STARDUST_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("STARDUST_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("STARDUST_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("STARDUST_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
