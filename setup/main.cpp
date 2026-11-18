// SPDX-License-Identifier: GPL-2.0-or-later
// stardust-setup.exe: sets up the release folder it sits in (tools/package.py) from the player's
// Super Stardust Ultra VR dump, so the game starts from stardust.exe or a desktop shortcut.
//
// With no arguments it opens a window: the game folder (the dump), the save folder, whether to skip the intro logos (settings.ini skip_intro), Install and Exit; after an
// install it offers a desktop shortcut. Run it again to change a folder, or after unpacking a newer
// release over the folder.
// With arguments it installs from the command line:
//
//     stardust-setup.exe --game <dump or eboot.bin>
//                           [--save <folder>] [--copy-saves] [--skip-intro|--play-intro]
//                           [--shortcut <folder>|desktop] [--accept-untested] [--progress]
//
// With --patch (stardust.exe runs it when game/ isn't prepared for this release or this dump's
// eboot.bin) it prepares the game again from paths.ini's folders in a small progress window.

#include "setup.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Super Stardust Ultra VR setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

// --- command line -------------------------------------------------------------------------------

// Writes to the standard handles a parent gave us, or else to the console of the shell we were started from.
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        err_ = GetStdHandle(STD_ERROR_HANDLE);
        if ((!out_ || !err_) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (!out_)
                out_ = con;
            if (!err_)
                err_ = con;
        }
    }
    void out(const std::wstring& s) const { write(out_, s + L"\n"); }
    void err(const std::wstring& s) const { write(err_, s + L"\n"); }

private:
    static void write(HANDLE h, const std::wstring& s) {
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode, n;
        if (GetConsoleMode(h, &mode)) {
            WriteConsoleW(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), len, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

constexpr wchar_t kUsage[] =
    L"usage: stardust-setup.exe --game <dump or eboot.bin>\n"
    L"                             [--save <folder>] [--copy-saves] [--skip-intro|--play-intro]\n"
    L"                             [--shortcut <folder>|desktop] [--accept-untested] [--progress]\n"
    L"  --game       the decrypted game dump (eboot.bin, sce_module, sce_sys, stream.dat); only ever read\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save\\ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --skip-intro, --play-intro\n"
    L"                skip the start-up logos and notices, or show them again\n"
    L"                (settings.ini skip_intro; default: leave it as it is)\n"
    L"  --shortcut    also write the game's shortcut into this folder, or onto the Desktop\n"
    L"  --accept-untested\n"
    L"                set up a dump that isn't the one this release was tested with (CUSA05951 1.00)\n"
    L"  --progress    also print \"progress <0..1> <step>\" lines (for launchers such as RecompLauncher)\n"
    L"  --patch      prepare the game again from paths.ini's folders (stardust.exe runs this)\n"
    L"Sets up the folder this exe is in. With no arguments, opens the setup window.";

int run_cli(const std::vector<std::wstring>& args) {
    Console con;
    std::wstring game, save, shortcut;
    bool copy_saves = false, accept_untested = false, progress = false;
    std::optional<bool> skip_intro;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--accept-untested") {
            accept_untested = true;
        } else if (a == L"--progress") {
            progress = true;
        } else if (a == L"--skip-intro" || a == L"--play-intro") {
            skip_intro = a == L"--skip-intro";
        } else {
            con.err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        con.err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        if (setup::missing_game_files(game).empty() && !accept_untested)
            if (std::wstring warning = setup::untested_warning(game); !warning.empty()) {
                con.err(warning + L"\n\nAdd --accept-untested to set it up anyway.");
                return 1;
            }
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = game;
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? fs::path(save) : old ? old->save : setup::default_save();
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_intro = skip_intro;
        fs::path exe = setup::install(opt, [&](const std::wstring& line) {
            con.out(line);
            if (std::wstring step = progress ? setup::progress_line(line) : std::wstring(); !step.empty())
                con.out(step);
        });
        if (!shortcut.empty()) {
            fs::path lnk = (shortcut == L"desktop" ? setup::desktop_folder() : fs::path(shortcut)) / setup::kShortcutName;
            setup::create_shortcut(exe, lnk);
            con.out(L"shortcut " + lnk.wstring());
        }
        con.out(L"\nDone. Run " + exe.wstring());
        return 0;
    } catch (const setup::Error& e) {
        con.err(e.message);
        return 1;
    }
}

// --- window -------------------------------------------------------------------------------------
// Drawn in the colours of the game's Display Settings panel (F10; gpu/shim/stardust_overlay.cpp
// PageInk, as they show on screen): a framed dark-teal panel on deep blue, teal rows, and the
// highlighted cyan for the action that is ready. Install stays disabled until the game folder holds a
// Super Stardust Ultra VR dump and a save folder is named.

enum : int {
    kGameEdit = 101,
    kGameBrowse,
    kSaveEdit,
    kSaveBrowse,
    kPlay,
    kSkipIntro,
};
enum : UINT {
    kMsgLog = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,             // wParam: 1 ok; lParam: std::wstring* exe path or error
};

// The look of the game's menus (gpu/shim/stardust_page.h, which the in-game pages and settings.exe
// share): deep space behind, frames in a glowing pale blue line with rounded corners over dark
// glass, chrome gold capitals with a dark rim, values and buttons in chrome silver, cyan accents.
// Type: Bruno Ace (SIL OFL 1.1, embedded as a resource), the free face nearest the game's.
struct Rgb {
    int r, g, b;
};
struct Chrome {
    Rgb top, horizon, low, bottom; // upper half top..horizon, then a hard edge, lower half low..bottom
};
namespace color {
constexpr Rgb SkyTop{0x0b, 0x12, 0x26}, SkyBottom{0x1c, 0x24, 0x3e}, Star{0xe8, 0xf0, 0xff};
constexpr Rgb Nebula{0x6a, 0x7c, 0xb8}, GlassTop{0x0a, 0x14, 0x2c}, GlassBottom{0x04, 0x08, 0x16};
constexpr Rgb Frame{0xcf, 0xec, 0xff}, Glow{0x58, 0xb4, 0xff}, Accent{0x6c, 0xdc, 0xff}, Rim{0x0a, 0x0b, 0x10};
constexpr Rgb Field{0x03, 0x07, 0x12}, Text{0xf2, 0xf6, 0xfa}, Help{0xb4, 0xc4, 0xd6}, Dim{0x5e, 0x68, 0x74};
constexpr Rgb Good{0x8c, 0xe4, 0xff}, Bad{0xff, 0x9a, 0x6c};
constexpr Chrome Gold{{0xff, 0xf6, 0xd6}, {0xe6, 0xc0, 0x74}, {0x9c, 0x6c, 0x2a}, {0xf2, 0xd6, 0x92}};
constexpr Chrome Silver{{0xff, 0xff, 0xff}, {0xc6, 0xd0, 0xda}, {0x74, 0x80, 0x8e}, {0xe2, 0xe8, 0xee}};
constexpr Chrome Steel{{0x86, 0x90, 0x9c}, {0x5e, 0x68, 0x74}, {0x36, 0x3e, 0x48}, {0x56, 0x60, 0x6c}};
COLORREF ref(Rgb c) {
    return RGB(c.r, c.g, c.b);
}
} // namespace color

// One folder row: its label, the field and Browse, and what the check says about the folder.
struct Folder {
    const wchar_t* label;
    int y;
    HWND edit = nullptr, browse = nullptr;
    std::wstring note;
    bool ok = false;
};

// A 32-bit picture to paint in, with its pixels at hand (0x00RRGGBB, top row first).
class Canvas {
public:
    Canvas(int width, int height) : w(std::max(1, width)), h(std::max(1, height)) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        void* bits = nullptr;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        px = static_cast<uint32_t*>(bits);
        old = SelectObject(dc, bmp);
    }
    ~Canvas() {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void blend(int x, int y, Rgb c, float a) {
        if (!px || x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f)
            return;
        a = std::min(a, 1.0f);
        uint32_t& p = px[static_cast<size_t>(y) * w + x];
        auto mix = [a](uint32_t from, int to) {
            return static_cast<uint32_t>(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * a + 0.5f);
        };
        p = mix((p >> 16) & 0xff, c.r) << 16 | mix((p >> 8) & 0xff, c.g) << 8 | mix(p & 0xff, c.b);
    }

    int w, h;
    HDC dc;
    uint32_t* px = nullptr;

private:
    HBITMAP bmp;
    HGDIOBJ old;
};

struct Ui {
    HWND wnd = nullptr, play = nullptr, install = nullptr, exit = nullptr, skip_intro = nullptr;
    Folder game{L"GAME FOLDER", 96}, save{L"SAVE FOLDER", 176};
    std::vector<HWND> inputs; // disabled while installing (Play Game too, and until the folder is set up)
    HFONT edit_font = nullptr, label = nullptr, small = nullptr, button = nullptr, title = nullptr, kicker = nullptr;
    HBRUSH field = nullptr;
    HANDLE face = nullptr;            // Bruno Ace, loaded for this process only
    std::unique_ptr<Canvas> backdrop; // the sky and the frame, under everything
    UINT dpi = 96;
    bool busy = false, skip = true;
    std::wstring status;
    bool status_bad = false;
    fs::path root;
};
Ui ui;

// Layout in 96-dpi units; the client area is kWidth x kHeight.
constexpr int kWidth = 640, kHeight = 412;
constexpr int kLabelWidth = 150; // the row's label ("GAME FOLDER"); its note fills the rest
constexpr int kRowLeft = 36, kRowRight = 604, kFieldLeft = 52, kBrowseLeft = 486, kBrowseWidth = 100;
constexpr int kPanelTop = 80, kPanelBottom = 310, kButtonsTop = 360;

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}
float pxf(float v) {
    return v * static_cast<float>(ui.dpi) / 96.0f;
}

RECT rect(int x, int y, int w, int h) {
    return {px(x), px(y), px(x + w), px(y + h)};
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    size_t a = s.find_first_not_of(L" \t\""), b = s.find_last_not_of(L" \t\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// --- drawing ---

Rgb lerp(Rgb a, Rgb b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {static_cast<int>(static_cast<float>(a.r) + static_cast<float>(b.r - a.r) * t),
            static_cast<int>(static_cast<float>(a.g) + static_cast<float>(b.g - a.g) * t),
            static_cast<int>(static_cast<float>(a.b) + static_cast<float>(b.b - a.b) * t)};
}

// Signed distance from (x, y) to a rounded rectangle: negative inside.
float box_distance(float x0, float y0, float x1, float y1, float r, float x, float y) {
    const float qx = std::fabs(x - (x0 + x1) * 0.5f) - ((x1 - x0) * 0.5f - r);
    const float qy = std::fabs(y - (y0 + y1) * 0.5f) - ((y1 - y0) * 0.5f - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// A frame as the game's: dark glass (a vertical gradient at `glass` opacity), a pale line with
// rounded corners just inside the edge, and a soft halo of `glow` around it. Antialiased; sizes in
// 96-dpi pixels.
struct FrameStyle {
    Rgb top = color::GlassTop, bottom = color::GlassBottom;
    float glass = 0.78f;
    Rgb line = color::Frame;
    float line_width = 1.5f, line_alpha = 1.0f;
    Rgb glow = color::Glow;
    float glow_radius = 9.0f, glow_alpha = 0.32f;
    float radius = 10.0f;
};
void frame(Canvas& cv, RECT r, const FrameStyle& st) {
    const float x0 = static_cast<float>(r.left), y0 = static_cast<float>(r.top), x1 = static_cast<float>(r.right),
                y1 = static_cast<float>(r.bottom);
    const float rad = pxf(st.radius), lw = pxf(st.line_width), gr = pxf(st.glow_radius);
    const int m = static_cast<int>(gr) + 2;
    for (int y = r.top - m; y < r.bottom + m; ++y) {
        for (int x = r.left - m; x < r.right + m; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float d = box_distance(x0, y0, x1, y1, rad, fx, fy);
            if (d > std::max(gr, 1.0f))
                continue;
            if (st.glass > 0.0f && d < 0.5f)
                cv.blend(x, y, lerp(st.top, st.bottom, (fy - y0) / (y1 - y0)), std::clamp(0.5f - d, 0.0f, 1.0f) * st.glass);
            if (gr > 0.0f && d > -lw)
                cv.blend(x, y, st.glow,
                         st.glow_alpha * std::exp(-std::max(d, 0.0f) / (gr * 0.38f)) * std::clamp(d + lw + 0.5f, 0.0f, 1.0f));
            const float cover = std::clamp(lw * 0.5f + 0.5f - std::fabs(d + lw * 0.5f), 0.0f, 1.0f);
            cv.blend(x, y, st.line, cover * st.line_alpha);
        }
    }
}

// A thick antialiased stroke from a to b.
void stroke(Canvas& cv, float ax, float ay, float bx, float by, float width, Rgb c) {
    const int x0 = static_cast<int>(std::min(ax, bx) - width) - 1, x1 = static_cast<int>(std::max(ax, bx) + width) + 1;
    const int y0 = static_cast<int>(std::min(ay, by) - width) - 1, y1 = static_cast<int>(std::max(ay, by) + width) + 1;
    const float dx = bx - ax, dy = by - ay, len2 = std::max(dx * dx + dy * dy, 1e-6f);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float qx = static_cast<float>(x) + 0.5f - ax, qy = static_cast<float>(y) + 0.5f - ay;
            const float t = std::clamp((qx * dx + qy * dy) / len2, 0.0f, 1.0f);
            const float ex = qx - dx * t, ey = qy - dy * t;
            cv.blend(x, y, c, std::clamp(width * 0.5f + 0.5f - std::sqrt(ex * ex + ey * ey), 0.0f, 1.0f));
        }
    }
}

// Deep space: a blue-grey sky, a faint nebula and a fixed scatter of stars.
void sky(Canvas& cv) {
    for (int y = 0; y < cv.h; ++y) {
        const Rgb c = lerp(color::SkyTop, color::SkyBottom, static_cast<float>(y) / static_cast<float>(cv.h));
        for (int x = 0; x < cv.w; ++x)
            cv.px[static_cast<size_t>(y) * cv.w + x] = static_cast<uint32_t>(c.r << 16 | c.g << 8 | c.b);
    }
    const float nx = static_cast<float>(cv.w) * 0.72f, ny = static_cast<float>(cv.h) * 0.38f, nr = static_cast<float>(cv.h) * 0.55f;
    for (int y = 0; y < cv.h; ++y) {
        for (int x = 0; x < cv.w; ++x) {
            const float d = std::hypot(static_cast<float>(x) - nx, (static_cast<float>(y) - ny) * 1.4f) / nr;
            if (d < 1.0f)
                cv.blend(x, y, color::Nebula, 0.10f * (1.0f - d) * (1.0f - d));
        }
    }
    uint32_t seed = 0x5d3a9e1u;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < 420; ++i) {
        const float x = next() * static_cast<float>(cv.w), y = next() * static_cast<float>(cv.h), bright = next();
        const float r = pxf(0.45f + bright * bright * 1.2f);
        for (int sy = static_cast<int>(y - r - 1); sy <= static_cast<int>(y + r + 1); ++sy) {
            for (int sx = static_cast<int>(x - r - 1); sx <= static_cast<int>(x + r + 1); ++sx) {
                const float d = std::hypot(static_cast<float>(sx) + 0.5f - x, static_cast<float>(sy) + 0.5f - y);
                cv.blend(sx, sy, color::Star, (0.25f + 0.75f * bright) * std::clamp(r + 0.5f - d, 0.0f, 1.0f));
            }
        }
    }
}

// Text as the game's menus draw it, antialiased: rendered as a coverage mask, then composited with
// a drop shadow and a dark rim (when `rim`) and filled with `chrome` (shaded apart at the capitals'
// middle) or a flat colour. Takes DrawTextW's format: alignment, word breaks, ellipsis.
struct TextStyle {
    const Chrome* chrome = nullptr;
    Rgb flat = color::Text;
    bool rim = false, bold = false;
};
void text(Canvas& cv, const std::wstring& s, RECT r, HFONT font, const TextStyle& st, UINT format) {
    if (s.empty())
        return;
    const int pad = px(4), w = r.right - r.left + pad * 2, h = r.bottom - r.top + pad * 2;
    if (w <= pad * 2 || h <= pad * 2)
        return;
    Canvas mask(w, h);
    if (!mask.px)
        return;
    std::fill(mask.px, mask.px + static_cast<size_t>(w) * h, 0u);
    HGDIOBJ old = SelectObject(mask.dc, font);
    SetBkMode(mask.dc, TRANSPARENT);
    SetTextColor(mask.dc, RGB(255, 255, 255));
    RECT inner = {pad, pad, pad + r.right - r.left, pad + r.bottom - r.top};
    DrawTextW(mask.dc, s.c_str(), static_cast<int>(s.size()), &inner, format | DT_NOPREFIX);
    SelectObject(mask.dc, old);
    GdiFlush();
    auto level = [](uint32_t p) {
        return static_cast<float>(std::max({(p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff})) / 255.0f;
    };
    std::vector<float> a(static_cast<size_t>(w) * h);
    int ink_top = h, ink_bottom = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float v = level(mask.px[i]);
            if (st.bold && x > 0)
                v = std::max(v, level(mask.px[i - 1]));
            a[i] = v;
            if (v > 0.5f) {
                ink_top = std::min(ink_top, y);
                ink_bottom = std::max(ink_bottom, y);
            }
        }
    }
    const int ox = r.left - pad, oy = r.top - pad;
    auto at = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : a[static_cast<size_t>(y) * w + x]; };
    if (st.rim) {
        const int rim = std::max(1, px(1)), drop = rim + std::max(1, px(1));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float v = 0.0f;
                for (int dy = -rim; dy <= rim; ++dy)
                    for (int dx = -rim; dx <= rim; ++dx)
                        v = std::max(v, at(x + dx, y + dy));
                cv.blend(ox + x, oy + y, color::Rim, std::max(v * 0.94f, at(x - drop, y - drop) * 0.55f));
            }
        }
    }
    const float top = static_cast<float>(ink_top), bottom = static_cast<float>(ink_bottom);
    const float horizon = top + (bottom - top) * 0.52f;
    for (int y = 0; y < h; ++y) {
        Rgb c = st.flat;
        if (st.chrome) {
            const Chrome& k = *st.chrome;
            const float fy = static_cast<float>(y);
            c = fy < horizon ? lerp(k.top, k.horizon, (fy - top) / std::max(1.0f, horizon - top))
                             : lerp(k.low, k.bottom, (fy - horizon) / std::max(1.0f, bottom - horizon));
        }
        for (int x = 0; x < w; ++x)
            cv.blend(ox + x, oy + y, c, a[static_cast<size_t>(y) * w + x]);
    }
}

// The window's backdrop: the sky and the frame around the folders. Built once.
void build_backdrop(int w, int h) {
    ui.backdrop = std::make_unique<Canvas>(w, h);
    if (!ui.backdrop->px)
        return;
    sky(*ui.backdrop);
    frame(*ui.backdrop, rect(18, kPanelTop, kWidth - 36, kPanelBottom - kPanelTop), FrameStyle{});
}

// The box the edit sits in: a dark field framed in a thin pale line.
RECT field_box(const Folder& f) {
    return rect(kFieldLeft, f.y + 34, kBrowseLeft - kFieldLeft - 12, 30);
}

void paint_folder(Canvas& cv, const Folder& f) {
    text(cv, f.label, rect(kFieldLeft, f.y + 4, kLabelWidth, 24), ui.label, {&color::Gold, color::Text, true, true},
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(cv, f.note, rect(kFieldLeft + kLabelWidth, f.y + 4, kRowRight - 18 - kFieldLeft - kLabelWidth, 24), ui.small,
         {nullptr, f.ok ? color::Good : color::Bad, true, false}, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    FrameStyle st;
    st.top = st.bottom = color::Field;
    st.glass = 1.0f;
    st.line_width = 1.0f;
    st.line_alpha = 0.55f;
    st.glow_radius = 0.0f;
    st.radius = 6.0f;
    frame(cv, field_box(f), st);
}

void paint(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    Canvas cv(client.right, client.bottom);
    BitBlt(cv.dc, 0, 0, cv.w, cv.h, ui.backdrop->dc, 0, 0, SRCCOPY);
    GdiFlush();
    text(cv, L"SUPER STARDUST ULTRA VR", rect(0, 8, kWidth, 22), ui.kicker, {&color::Silver, color::Text, true, false},
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(cv, L"SETUP", rect(0, 28, kWidth, 46), ui.title, {&color::Gold, color::Text, true, true},
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const Folder* folders[] = {&ui.game, &ui.save};
    for (const Folder* f : folders)
        paint_folder(cv, *f);
    for (int i = 1; i < 2; ++i) { // faint rules between the rows
        const float y = pxf(static_cast<float>(folders[i]->y - 9));
        stroke(cv, pxf(kRowLeft), y, pxf(kRowRight), y, 1.0f, lerp(color::GlassTop, color::Frame, 0.18f));
    }
    {
        const float y = pxf(static_cast<float>(ui.save.y + 77));
        stroke(cv, pxf(kRowLeft), y, pxf(kRowRight), y, 1.0f, lerp(color::GlassTop, color::Frame, 0.18f));
    }
    text(cv, ui.status, rect(24, kPanelBottom + 8, kWidth - 48, kButtonsTop - kPanelBottom - 10), ui.small,
         {nullptr, ui.status_bad ? color::Bad : color::Help, true, false}, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    BitBlt(dc, 0, 0, cv.w, cv.h, cv.dc, 0, 0, SRCCOPY);
}

// Owner-drawn controls on the backdrop: buttons as small glass frames with chrome labels (gold for
// the one action that is ready, steel when disabled, a cyan line when focused), the intro box as a
// framed square with a cyan tick.
void draw_button(const DRAWITEMSTRUCT& d) {
    RECT r = d.rcItem;
    Canvas cv(r.right - r.left, r.bottom - r.top);
    if (!cv.px)
        return;
    RECT on_parent = r;
    MapWindowPoints(d.hwndItem, ui.wnd, reinterpret_cast<POINT*>(&on_parent), 2);
    BitBlt(cv.dc, 0, 0, cv.w, cv.h, ui.backdrop->dc, on_parent.left, on_parent.top, SRCCOPY);
    GdiFlush();
    const bool disabled = d.itemState & ODS_DISABLED, pressed = d.itemState & ODS_SELECTED, focus = d.itemState & ODS_FOCUS;
    wchar_t label[128] = {};
    GetWindowTextW(d.hwndItem, label, 128);
    if (d.hwndItem == ui.skip_intro) {
        const int s = px(18), top = (cv.h - s) / 2;
        const RECT box = {px(3), top, px(3) + s, top + s};
        FrameStyle st;
        st.top = st.bottom = color::Field;
        st.glass = 1.0f;
        st.line = focus ? color::Accent : color::Frame;
        st.line_width = focus ? 2.0f : 1.2f;
        st.glow_radius = focus ? 6.0f : 0.0f;
        st.radius = 4.0f;
        frame(cv, box, st);
        if (ui.skip) {
            const float x = static_cast<float>(box.left), y = static_cast<float>(box.top), u = static_cast<float>(s) / 18.0f;
            stroke(cv, x + 4 * u, y + 9.5f * u, x + 7.5f * u, y + 13 * u, 2.2f * u, color::Accent);
            stroke(cv, x + 7.5f * u, y + 13 * u, x + 14 * u, y + 5 * u, 2.2f * u, color::Accent);
        }
        RECT t = {box.right + px(10), 0, cv.w, cv.h};
        text(cv, label, t, ui.small, {nullptr, disabled ? color::Dim : color::Help, true, false},
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        const bool primary = d.hwndItem == (setup::ready(ui.root) && !IsWindowEnabled(ui.install) ? ui.play : ui.install);
        const int m = px(5);
        FrameStyle st;
        st.glass = pressed ? 0.95f : 0.8f;
        if (pressed)
            std::swap(st.top, st.bottom);
        st.line = disabled ? color::Dim : focus ? color::Accent : color::Frame;
        st.line_alpha = disabled ? 0.6f : 1.0f;
        st.line_width = focus ? 2.0f : 1.2f;
        st.glow_radius = disabled ? 0.0f : 5.0f;
        st.glow_alpha = focus || primary ? 0.45f : 0.22f;
        st.radius = 8.0f;
        frame(cv, {m, m, cv.w - m, cv.h - m}, st);
        RECT t = {0, 0, cv.w, cv.h};
        if (pressed)
            OffsetRect(&t, 0, px(1));
        const bool browse = d.CtlID == kGameBrowse || d.CtlID == kSaveBrowse;
        text(cv, label, t, browse ? ui.small : ui.button,
             {disabled ? &color::Steel : primary ? &color::Gold : &color::Silver, color::Text, true, false},
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(d.hDC, r.left, r.top, cv.w, cv.h, cv.dc, 0, 0, SRCCOPY);
}

// --- state ---

void set_status(std::wstring s, bool bad = false) {
    ui.status = std::move(s);
    ui.status_bad = bad;
    RECT r = rect(0, kPanelBottom + 4, kWidth, kButtonsTop - kPanelBottom - 6);
    InvalidateRect(ui.wnd, &r, FALSE);
}

// Checks both folders, shows the result in each row and enables Install when all are usable.
void validate() {
    std::error_code ec;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit);
    ui.game.ok = false;
    if (game.empty()) {
        ui.game.note = L"Required: your decrypted dump";
    } else if (!fs::is_directory(setup::game_folder(game), ec)) {
        ui.game.note = L"Folder not found";
    } else if (auto missing = setup::missing_game_files(game); !missing.empty()) {
        ui.game.note = L"Not the game's dump: no " + fs::path(missing.front()).filename().wstring();
    } else {
        ui.game.ok = true;
        ui.game.note = L"Super Stardust Ultra VR dump found";
    }
    ui.save.ok = !save.empty();
    ui.save.note = !ui.save.ok                         ? L"Required"
                   : fs::is_directory(fs::path(save), ec) ? L"Folder found"
                                                        : L"Created when you install";
    EnableWindow(ui.install, !ui.busy && ui.game.ok && ui.save.ok);
    InvalidateRect(ui.wnd, nullptr, FALSE);
    InvalidateRect(ui.install, nullptr, FALSE);
    InvalidateRect(ui.play, nullptr, FALSE);
}

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    EnableWindow(ui.play, !busy && setup::ready(ui.root));
    validate();
}

// Starts the game as a double-click would (its own folder, no console), then closes the setup.
void play() {
    fs::path exe = ui.root / L"stardust.exe";
    auto started = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ui.wnd, L"open", exe.c_str(), nullptr, ui.root.c_str(), SW_SHOWNORMAL));
    if (started <= 32) {
        MessageBoxW(ui.wnd, (L"Cannot start " + exe.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    DestroyWindow(ui.wnd);
}

// The shell's folder picker, starting in the folder the field names when it exists.
void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring current = text_of(edit);
    if (!current.empty()) {
        std::error_code ec;
        fs::path start = setup::game_folder(current);
        IShellItem* item = nullptr;
        if (fs::is_directory(start, ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path); // EN_CHANGE validates it
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
}

void start_install() {
    if (ui.busy || !IsWindowEnabled(ui.install))
        return;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit);
    setup::Options opt;
    opt.root = ui.root;
    opt.game = game;
    opt.save = save;
    opt.skip_intro = ui.skip;
    if (std::wstring warning = setup::untested_warning(game); !warning.empty()) {
        warning += L"\n\nInstall with these files anyway?";
        if (MessageBoxW(ui.wnd, warning.c_str(), kTitle, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
    }
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            std::wstring ask = L"Copy your saves to the new save folder?\n\nFrom: " + old->wstring() + L"\nTo: " + save;
            int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                opt.copy_saves_from = old;
        }
    } catch (const setup::Error& e) {
        MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Installing...");
    set_busy(true);
    std::thread([opt, wnd = ui.wnd] {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
}

void finish_install(bool ok, const std::wstring& value) {
    set_busy(false);
    if (!ok) {
        set_status(L"Install failed. Check the folders and press Install again.", true);
        MessageBoxW(ui.wnd, value.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Done. Press Play Game to start.");
    if (MessageBoxW(ui.wnd, L"Done!\n\nAdd a shortcut to the game on your desktop?", kTitle,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        try {
            setup::create_shortcut(value, setup::desktop_folder() / setup::kShortcutName);
            set_status(L"Done, with a desktop shortcut. Press Play Game to start.");
        } catch (const setup::Error& e) {
            MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    SetFocus(ui.play); // Enter now starts the game
}

// --- controls ---

HWND add(const wchar_t* cls, const wchar_t* label, DWORD style, RECT r, int id) {
    HWND c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, ui.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.edit_font), FALSE);
    return c;
}

void add_folder(Folder& f, int edit_id, int browse_id) {
    RECT box = field_box(f);
    // The edit inside its drawn box, its text centred on the box's height.
    int h = px(18);
    RECT r = {box.left + px(9), box.top + (box.bottom - box.top - h) / 2, box.right - px(9), 0};
    r.bottom = r.top + h;
    f.edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, r, edit_id);
    f.browse = add(WC_BUTTONW, L"BROWSE", WS_TABSTOP | BS_OWNERDRAW, rect(kBrowseLeft - 4, f.y + 30, kBrowseWidth + 8, 38),
                   browse_id);
}

void create_controls() {
    // Tab order: the folders top to bottom, the intro box, then the buttons.
    add_folder(ui.game, kGameEdit, kGameBrowse);
    add_folder(ui.save, kSaveEdit, kSaveBrowse);
    ui.skip_intro = add(WC_BUTTONW, L"Skip the intro movies (settings.exe can bring them back)",
                        WS_TABSTOP | BS_OWNERDRAW, rect(kFieldLeft - 3, 264, kRowRight - kFieldLeft - 8, 30), kSkipIntro);
    ui.play = add(WC_BUTTONW, L"PLAY GAME", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 22 - 3 * 150, kButtonsTop, 146, 44), kPlay);
    ui.install = add(WC_BUTTONW, L"INSTALL", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 22 - 2 * 150, kButtonsTop, 146, 44), IDOK);
    ui.exit = add(WC_BUTTONW, L"EXIT", WS_TABSTOP | BS_OWNERDRAW, rect(kWidth - 22 - 150, kButtonsTop, 146, 44), IDCANCEL);
    ui.inputs = {ui.game.edit, ui.game.browse, ui.save.edit, ui.save.browse,
                 ui.skip_intro, ui.exit};

    // Start from what the last install chose (paths.ini here), else an empty game folder and save\ here.
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    fs::path save = paths ? paths->save : setup::default_save();
    if (paths) {
        SetWindowTextW(ui.game.edit, paths->game.c_str());
    }
    SetWindowTextW(ui.save.edit, save.c_str());
    // What settings.ini says now; Install writes the box's state, so unticking it brings the logos back.
    ui.skip = setup::skip_intro(ui.root, save);
    set_status(setup::ready(ui.root) ? L"The game is set up. Press Play Game, or change the folders and press Install."
               : paths              ? L"This version isn't set up yet. Press Install."
                                    : L"Choose your game dump, then press Install.");
    set_busy(false);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) {
            validate();
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDOK:
            start_install();
            return 0;
        case kPlay:
            play();
            return 0;
        case IDCANCEL:
            if (!ui.busy)
                DestroyWindow(wnd);
            return 0;
        case kSkipIntro:
            ui.skip = !ui.skip;
            InvalidateRect(ui.skip_intro, nullptr, FALSE);
            return 0;
        case kGameBrowse:
            browse(ui.game.edit, L"Select the game folder (the one with eboot.bin)");
            return 0;
        case kSaveBrowse:
            browse(ui.save.edit, L"Select the save folder");
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: { // a disabled edit asks with WM_CTLCOLORSTATIC
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, color::ref(IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? color::Text : color::Dim));
        SetBkColor(dc, color::ref(color::Field));
        return reinterpret_cast<LRESULT>(ui.field);
    }
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_ERASEBKGND:
        return 1; // WM_PAINT draws everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT client;
        GetClientRect(wnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        set_status(*line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        finish_install(wp == 1, *value);
        return 0;
    }
    case WM_CLOSE:
        if (!ui.busy)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Bruno Ace at `height` (96-dpi pixels): antialiased in grey for the painted text, ClearType in the
// edits. Falls back to the system's font if the embedded face cannot be loaded.
HFONT make_font(int height, BYTE quality) {
    LOGFONTW lf = {};
    lf.lfHeight = -px(height);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = quality;
    if (ui.face) {
        wcscpy_s(lf.lfFaceName, L"Bruno Ace");
    } else {
        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
        wcscpy_s(lf.lfFaceName, ncm.lfMessageFont.lfFaceName);
    }
    return CreateFontIndirectW(&lf);
}

// The embedded Bruno Ace (setup.rc, resource 2), for this process only.
void load_face(HINSTANCE inst) {
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL data = res ? LoadResource(inst, res) : nullptr;
    void* bytes = data ? LockResource(data) : nullptr;
    DWORD count = 0;
    if (bytes)
        ui.face = AddFontMemResourceEx(bytes, SizeofResource(inst, res), nullptr, &count);
}

int run_window(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    load_face(inst);
    ui.edit_font = make_font(13, CLEARTYPE_QUALITY);
    ui.small = make_font(12, ANTIALIASED_QUALITY);
    ui.label = make_font(15, ANTIALIASED_QUALITY);
    ui.button = make_font(16, ANTIALIASED_QUALITY);
    ui.kicker = make_font(13, ANTIALIASED_QUALITY);
    ui.title = make_font(36, ANTIALIASED_QUALITY);
    ui.field = CreateSolidBrush(color::ref(color::Field));
    build_backdrop(px(kWidth), px(kHeight));

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"stardust_setup";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // A dark title bar in the sky's colour (Windows 11; older versions ignore it).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::ref(color::SkyTop);
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    create_controls();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.game.edit);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg)) // Tab between controls, Enter = Install, Esc = Exit
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HFONT f : {ui.edit_font, ui.small, ui.label, ui.button, ui.kicker, ui.title})
        DeleteObject(f);
    DeleteObject(ui.field);
    ui.backdrop.reset();
    if (ui.face)
        RemoveFontMemResourceEx(ui.face);
    return 0;
}

// --- --patch ------------------------------------------------------------------------------------
// stardust.exe found game/ not prepared for this release or this dump's eboot.bin: prepare it again
// from paths.ini's folders, in a small window of the same look (the title, the step, a glowing bar).
// When the dump isn't the one this release was tested with, the player is asked first; cancelling,
// or a failed install (a message box says why), keeps the game from starting.

struct Patch {
    double done = 0;
    std::wstring step = L"Checking your game files";
    int result = -1; // the exit code once install() has ended
};
Patch patch;
constexpr int kPatchWidth = 520, kPatchHeight = 170;

void paint_patch(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    Canvas cv(client.right, client.bottom);
    BitBlt(cv.dc, 0, 0, cv.w, cv.h, ui.backdrop->dc, 0, 0, SRCCOPY);
    GdiFlush();
    text(cv, L"SUPER STARDUST ULTRA VR", rect(0, 10, kPatchWidth, 22), ui.kicker, {&color::Silver, color::Text, true, false},
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(cv, L"PREPARING", rect(0, 30, kPatchWidth, 40), ui.title, {&color::Gold, color::Text, true, true},
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(cv, patch.step, rect(32, 82, kPatchWidth - 64, 24), ui.small, {nullptr, color::Help, true, false},
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const RECT bar = rect(32, 114, kPatchWidth - 64, 20);
    const LONG fill = static_cast<LONG>((bar.right - bar.left) * std::clamp(patch.done, 0.0, 1.0));
    if (fill > px(8)) {
        FrameStyle st;
        st.top = color::Accent;
        st.bottom = color::Glow;
        st.glass = 0.9f;
        st.line = color::Accent;
        st.glow_radius = 6.0f;
        st.radius = 6.0f;
        frame(cv, {bar.left, bar.top, bar.left + fill, bar.bottom}, st);
    }
    FrameStyle outline;
    outline.glass = 0.0f;
    outline.line_width = 1.2f;
    outline.glow_radius = 0.0f;
    outline.radius = 6.0f;
    frame(cv, bar, outline);
    BitBlt(dc, 0, 0, cv.w, cv.h, cv.dc, 0, 0, SRCCOPY);
}

LRESULT CALLBACK patch_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        paint_patch(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgLog: { // "progress <0..1> <step>" for the lines that start a step
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        std::wstring step = setup::progress_line(*line);
        if (step.empty())
            return 0;
        size_t space = step.find(L' ', 9);
        patch.done = _wtof(step.substr(9, space - 9).c_str());
        patch.step = space == std::wstring::npos ? L"" : step.substr(space + 1);
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        patch.result = wp == 1 ? 0 : 1;
        if (wp != 1)
            MessageBoxW(wnd, value->c_str(), kTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(wnd);
        return 0;
    }
    case WM_CLOSE:
        return 0; // until install() ends
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int run_patch(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    std::optional<setup::Paths> paths = setup::remembered_paths(ui.root);
    if (!paths) {
        MessageBoxW(nullptr, L"The game is not set up yet. Run stardust-setup.exe.", kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (std::wstring warning = setup::untested_warning(paths->game); !warning.empty()) {
        warning += L"\n\nPrepare the game with these files and play anyway?";
        if (MessageBoxW(nullptr, warning.c_str(), kTitle, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return 1;
    }
    setup::Options opt;
    opt.root = ui.root;
    opt.game = paths->game;
    opt.save = paths->save;

    ui.dpi = GetDpiForSystem();
    load_face(inst);
    ui.small = make_font(13, ANTIALIASED_QUALITY);
    ui.kicker = make_font(13, ANTIALIASED_QUALITY);
    ui.title = make_font(30, ANTIALIASED_QUALITY);
    ui.backdrop = std::make_unique<Canvas>(px(kPatchWidth), px(kPatchHeight));
    if (ui.backdrop->px)
        sky(*ui.backdrop);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = patch_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"stardust_patch";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX;
    RECT r = {0, 0, px(kPatchWidth), px(kPatchHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = r.right - r.left, h = r.bottom - r.top;
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, work.left + (work.right - work.left - w) / 2,
                             work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::ref(color::SkyTop);
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    ShowWindow(ui.wnd, SW_SHOW);
    SetForegroundWindow(ui.wnd);

    std::thread([opt, wnd = ui.wnd] {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HFONT f : {ui.small, ui.kicker, ui.title})
        DeleteObject(f);
    ui.backdrop.reset();
    if (ui.face)
        RemoveFontMemResourceEx(ui.face);
    return patch.result < 0 ? 1 : patch.result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (args.size() == 1 && args[0] == L"--patch")
        return run_patch(inst);
    if (!args.empty())
        return run_cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
