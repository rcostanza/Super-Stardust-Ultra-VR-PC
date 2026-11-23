// SPDX-License-Identifier: GPL-2.0-or-later
#include "stardust_settings.h"
#include "stardust_graphics.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

double StardustWindowRefreshRate(); // stardust_gpu.cpp: the window's display, 0 when unknown

namespace StardustSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("STARDUST_CONFIG");
    return env && env[0] ? env : "settings.ini";
}

// The file read: Path() when it exists, else STARDUST_CONFIG_DEFAULT (the launcher's: the one the
// setup wrote next to the exe, while Path() is the save folder's). Writes always go to Path().
const char* ReadPath() {
    if (FILE* file = std::fopen(Path(), "r")) {
        std::fclose(file);
        return Path();
    }
    const char* fallback = std::getenv("STARDUST_CONFIG_DEFAULT");
    return fallback && fallback[0] ? fallback : Path();
}

bool Truthy(std::string_view value) {
    std::string lower;
    for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const int i = std::atoi(value.c_str());
    if (key == "display_mode") {
        v.display_mode = StardustGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = Truthy(value);
    } else if (key == "tearing") {
        v.tearing = Truthy(value);
    } else if (key == "monitor") {
        v.monitor = StardustGraphics::ParseMonitor(value);
    } else if (key == "output_resolution" || key == "window") { // window: its name before Super Stardust Ultra VR
        const auto [w, h] = StardustGraphics::ParseWindowSize(value);
        v.window_width = w;
        v.window_height = h;
    } else if (key == "aspect_ratio") {
        // Older settings files: the game draws 16:9 only (runtime_game_widen), so the key is ignored.
    } else if (key == "skip_intro") {
        v.skip_intro = Truthy(value);
    } else if (key == "sharpening") {
        v.sharpening = Truthy(value);
    } else if (key == "antialiasing") {
        v.antialiasing = Truthy(value);
    } else if (key == "cheats") {
        v.cheats = std::clamp(i, 0, 31);
    } else if (key == "frame_rate") {
        const bool listed = StardustGraphics::IndexOf(StardustGraphics::FrameRates, i, -1) >= 0;
        // Above the top rate (240 was offered once) is the top rate: enemies stop spawning from 220.
        const int top = StardustGraphics::FrameRates.back();
        v.frame_rate = listed ? i : i > top ? top : 60;
        if (!value.empty() && !listed) {
            std::printf("Settings: frame_rate=%s is not 30, 60, 90, 120, 144 or 180; using %d\n", value.c_str(), v.frame_rate.load());
        }
    } else if (key == "audio_channels") {
        v.audio_channels = ParseAudioChannels(value);
    } else if (key == "pad_speaker") {
        v.pad_speaker = ParsePadSpeaker(value);
    } else if (key == "render_resolution") {
        std::string lower;
        for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
        int chosen = lower == "2160p" ? Render4k : -1;
        for (int r = 0; r < RenderResolutionCount; ++r)
            if (lower == RenderResolutionKeys[r]) chosen = r;
        if (chosen < 0) {
            std::printf("Settings: render_resolution=%s is not 720p, 1080p, 1440p or 4k; using 1080p\n", value.c_str());
            chosen = Render1080p;
        }
        v.startup_render_resolution = v.render_resolution = chosen;
    } else {
        std::printf("Settings: unknown key %s\n", key.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> DisplayKeys() {
    const auto& v = Get();
    const int monitor = v.monitor;
    return {
        {"display_mode", std::string(StardustGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)])},
        {"monitor", monitor < 0 ? std::string() : std::to_string(monitor)},
        {"output_resolution", std::to_string(v.window_width.load()) + "x" + std::to_string(v.window_height.load())},
        {"vsync", v.vsync ? "1" : "0"},
        {"tearing", v.tearing ? "1" : "0"},
        {"frame_rate", std::to_string(v.frame_rate.load())},
        {"render_resolution", std::string(RenderResolutionKeys[std::clamp(v.render_resolution.load(), 0, RenderResolutionCount - 1)])},
        {"sharpening", v.sharpening ? "1" : "0"},
        {"antialiasing", v.antialiasing ? "1" : "0"},
        {"cheats", std::to_string(v.cheats.load())},
    };
}

std::mutex save_mutex;
// The display keys as Path() holds them: loaded at start, updated by each save.
std::vector<std::pair<std::string, std::string>> saved_keys;

bool Exists(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file) std::fclose(file);
    return file != nullptr;
}

// save_mutex held. Writes the display keys into Path() in place, from ReadPath()'s lines.
void WriteDisplay() {
    auto keys = DisplayKeys();
    saved_keys = keys;
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(ReadPath(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            lines.push_back(text);
        }
        std::fclose(file);
    }
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        // An old window= line becomes output_resolution= (the same setting, renamed).
        const std::string name = line.substr(0, eq) == "window" ? "output_resolution" : line.substr(0, eq);
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return name == k.first; });
        if (key == keys.end()) continue;
        line = key->first + "=" + key->second;
        keys.erase(key);
    }
    for (const auto& [key, value] : keys) lines.push_back(key + "=" + value);
    FILE* file = std::fopen(Path(), "wb");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
    std::printf("Settings: saved display settings to %s\n", Path());
}

// Gives the save folder its own settings.ini at start: the default file copied as it is (the
// environment overrides stay out of it), or the display keys when there is none.
void CopyToSaveFolder() {
    const char* env = std::getenv("STARDUST_CONFIG"); /* the launcher's or run.py's save folder */
    if (!env || !env[0] || Exists(Path())) return;
    const char* source = ReadPath();
    if (std::string_view(source) == Path()) {
        WriteDisplay();
        return;
    }
    FILE* in = std::fopen(source, "rb");
    if (!in) return;
    std::string data;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;) data.append(buffer, n);
    std::fclose(in);
    FILE* out = std::fopen(Path(), "wb");
    if (!out) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    if (std::fclose(out) != 0 || !ok) {
        std::printf("Settings: cannot write %s\n", Path());
        std::remove(Path());
        return;
    }
    std::printf("Settings: copied %s to %s\n", source, Path());
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    const char* path = ReadPath();
    if (FILE* file = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", path);
        if (std::string_view(path) != Path()) std::printf("Settings: changes are saved to %s\n", Path());
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"STARDUST_DISPLAY_MODE", "display_mode"}, {"STARDUST_VSYNC", "vsync"},
        {"STARDUST_TEARING", "tearing"},
        {"STARDUST_MONITOR", "monitor"},           {"STARDUST_WINDOW", "output_resolution"},
        {"STARDUST_OUTPUT_RESOLUTION", "output_resolution"},
        {"STARDUST_AUDIO_CHANNELS", "audio_channels"}, {"STARDUST_PAD_SPEAKER", "pad_speaker"},
        {"STARDUST_SKIP_INTRO", "skip_intro"},     {"STARDUST_FRAME_RATE", "frame_rate"},
        {"STARDUST_RENDER_RESOLUTION", "render_resolution"},

        {"STARDUST_SHARPENING", "sharpening"},
        {"STARDUST_ANTIALIASING", "antialiasing"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_monitor = v.monitor;
    std::scoped_lock lock{save_mutex};
    saved_keys = DisplayKeys();
    CopyToSaveFolder();
}

void SaveDisplay() {
    std::scoped_lock lock{save_mutex};
    WriteDisplay();
}

void SaveDisplayIfChanged() {
    std::scoped_lock lock{save_mutex};
    if (DisplayKeys() != saved_keys) WriteDisplay();
}

double DisplayRefresh() {
    static std::atomic<double> refresh{0.0};
    static std::atomic<long long> next_check{0};
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now >= next_check.load(std::memory_order_relaxed)) {
        next_check.store(now + 500, std::memory_order_relaxed);
        refresh.store(StardustWindowRefreshRate(), std::memory_order_relaxed);
    }
    return refresh.load(std::memory_order_relaxed);
}

int EffectiveFrameRate() {
    const auto& v = Get();
    const int rate = v.frame_rate.load(std::memory_order_relaxed);
    const double refresh = DisplayRefresh();
    if ((!v.vsync && v.tearing) || refresh <= 0.0) return rate;
    int best = StardustGraphics::FrameRates[0];
    for (const int r : StardustGraphics::FrameRates) {
        if (r <= rate && r <= refresh + 0.5) best = std::max(best, r);
    }
    return best;
}

} // namespace StardustSettings

extern "C" int stardust_skip_intro(void) {
    return StardustSettings::Get().skip_intro ? 1 : 0;
}

extern "C" int stardust_frame_rate(void) {
    return StardustSettings::EffectiveFrameRate();
}

extern "C" void stardust_set_frame_rate(int rate) {
    StardustSettings::Get().frame_rate = rate;
}

// Super Stardust Ultra VR has no HDR output: it never asks the video out for BT.2020 PQ
// (sceVideoOutGetDeviceCapabilityInfo) and always configures SDR, and its param.sfo does not flag
// HDR. The presenter therefore always uses an SDR surface.
extern "C" int stardust_hdr(void) {
    return 0;
}

extern "C" int stardust_antialiasing(void) {
    return StardustSettings::Get().antialiasing.load(std::memory_order_relaxed);
}

extern "C" int stardust_width_4k(void) {
    return StardustSettings::Get().startup_width_4k.load();
}

extern "C" void stardust_set_width_4k(int width) {
    StardustSettings::Get().startup_width_4k = width;
}

extern "C" int stardust_render_4k(void) {
    return StardustSettings::Get().startup_render_resolution == StardustSettings::Render4k;
}

extern "C" int stardust_render_height(void) {
    const int r = StardustSettings::Get().startup_render_resolution;
    return StardustSettings::RenderResolutionHeights[std::clamp(r, 0, StardustSettings::RenderResolutionCount - 1)];
}
