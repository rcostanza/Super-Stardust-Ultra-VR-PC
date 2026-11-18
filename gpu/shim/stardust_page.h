// SPDX-License-Identifier: GPL-2.0-or-later
// The look of the runner's pages, drawn with Dear ImGui: shared by the in-game overlay's pages
// (stardust_overlay.cpp: Display Settings, the exit prompt) and settings.exe (settings/main.cpp), so
// both show the same Display Settings page. As Super Stardust Ultra VR's own menus: deep space
// behind (the game's frame under a dark veil, or a drawn starfield in settings.exe), wide squared
// capitals in chrome gold with a dark rim, the focused item marked by a glowing purple orb and a
// cyan arrow, values and button hints in chrome silver, panels framed in a glowing pale blue line
// with rounded corners, and button hints as silver-ringed pad glyphs at the bottom right. Type:
// Bruno Ace (SIL OFL 1.1), the free face nearest the game's.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // ImRect, ShadeVertsLinearColorGradientKeepAlpha

// Embeds a font file (third_party/fonts) as symbol[] .. symbol_end[]; used once per program.
#ifdef _WIN32
#define STARDUST_EMBED(symbol, path)                                                               \
    asm(".section .rdata,\"dr\"\n"                                                                 \
        ".balign 16\n"                                                                             \
        ".global " #symbol "\n" #symbol ":\n"                                                      \
        ".incbin \"" path "\"\n"                                                                   \
        ".global " #symbol "_end\n" #symbol "_end:\n"                                              \
        ".text\n");
#else
#define STARDUST_EMBED(symbol, path)                                                               \
    asm(".section .rodata\n"                                                                       \
        ".balign 16\n"                                                                             \
        ".hidden " #symbol "\n.global " #symbol "\n" #symbol ":\n"                                 \
        ".incbin \"" path "\"\n"                                                                   \
        ".hidden " #symbol "_end\n.global " #symbol "_end\n" #symbol "_end:\n"                     \
        ".previous\n");
#endif

namespace StardustPage {

struct Fonts {
    ImFont* face = nullptr; // Bruno Ace: every line on the pages
};

// A chrome fill: the upper half from `top` to `horizon`, then a hard edge, the lower half from
// `low` up to `bottom`, as the game's bevelled menu type.
struct Chrome {
    ImU32 top, horizon, low, bottom;
};

// The colours, given as seen (sRGB). The overlay draws to an sRGB image, so it takes Linear; a
// plain window (settings.exe) takes Srgb.
struct Ink {
    ImU32 veil_mid, veil_edge;      // over the game: darkest at the top and bottom
    ImU32 space_top, space_bottom;  // settings.exe's sky
    ImU32 nebula_a, nebula_b, star; // and what is in it
    ImU32 panel_top, panel_bottom;  // inside a frame
    ImU32 frame, frame_glow;        // the frame's line and its halo
    ImU32 rim;                      // the dark edge around chrome type
    Chrome gold, gold_dim, silver, steel;
    ImU32 text, text_dim, accent, band;
    ImU32 orb, orb_core;               // the cursor
    ImU32 ring, face, cross, circle, triangle; // pad glyphs
};
constexpr ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// sRGB to linear for one colour, alpha kept.
inline ImU32 ToLinear(ImU32 c) {
    auto channel = [&](int shift) {
        const float v = float((c >> shift) & 0xff) / 255.0f;
        const float l = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        return ImU32(std::lround(l * 255.0f)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
           (c & IM_COL32_A_MASK);
}

inline Ink MakeInk(bool linear) {
    auto c = [linear](int r, int g, int b, int a = 255) {
        const ImU32 v = Rgb(r, g, b, a);
        return linear ? ToLinear(v) : v;
    };
    Ink ink{};
    ink.veil_mid = c(0x06, 0x0c, 0x1e, 0xa0);
    ink.veil_edge = c(0x02, 0x04, 0x0c, 0xe0);
    ink.space_top = c(0x0b, 0x12, 0x26);
    ink.space_bottom = c(0x1c, 0x24, 0x3e);
    ink.nebula_a = c(0x6a, 0x7c, 0xb8, 0x0a);
    ink.nebula_b = c(0x8c, 0x74, 0xb0, 0x08);
    ink.star = c(0xe8, 0xf0, 0xff);
    ink.panel_top = c(0x0a, 0x14, 0x2c, 0xb8);
    ink.panel_bottom = c(0x04, 0x08, 0x16, 0xd8);
    ink.frame = c(0xcf, 0xec, 0xff);
    ink.frame_glow = c(0x58, 0xb4, 0xff, 0x46);
    ink.rim = c(0x0a, 0x0b, 0x10, 0xf0);
    ink.gold = {c(0xff, 0xf6, 0xd6), c(0xe6, 0xc0, 0x74), c(0x9c, 0x6c, 0x2a), c(0xf2, 0xd6, 0x92)};
    ink.gold_dim = {c(0xe4, 0xd8, 0xb8), c(0xbc, 0x9c, 0x60), c(0x76, 0x56, 0x2a), c(0xc8, 0xb0, 0x7c)};
    ink.silver = {c(0xff, 0xff, 0xff), c(0xc6, 0xd0, 0xda), c(0x74, 0x80, 0x8e), c(0xe2, 0xe8, 0xee)};
    ink.steel = {c(0x86, 0x90, 0x9c), c(0x5e, 0x68, 0x74), c(0x36, 0x3e, 0x48), c(0x56, 0x60, 0x6c)};
    ink.text = c(0xf2, 0xf6, 0xfa);
    ink.text_dim = c(0x9a, 0xaa, 0xbc);
    ink.accent = c(0x6c, 0xdc, 0xff);
    ink.band = c(0x6c, 0xc8, 0xff, 0x30);
    ink.orb = c(0xa8, 0x6c, 0xff);
    ink.orb_core = c(0xf6, 0xee, 0xff);
    ink.ring = c(0xc4, 0xcc, 0xd6);
    ink.face = c(0x1e, 0x26, 0x34);
    ink.cross = c(0x8c, 0xb6, 0xf2);
    ink.circle = c(0xf0, 0x6c, 0x7c);
    ink.triangle = c(0x5c, 0xd6, 0xb4);
    return ink;
}
inline const Ink Linear = MakeInk(true);
inline const Ink Srgb = MakeInk(false);

inline ImU32 Alpha(ImU32 c, float a) {
    const float old = float((c & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT);
    return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(old * a, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
}

inline std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

inline float Width(const Fonts& fonts, float size, const char* text) {
    return fonts.face->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

// Plain text with a soft dark shadow (help, notes).
inline void Text(ImDrawList* dl, const Ink& ink, const Fonts& fonts, float size, ImVec2 at, const char* text,
                 ImU32 color, float wrap = 0.0f) {
    const float d = std::max(1.0f, size * 0.07f);
    dl->AddText(fonts.face, size, ImVec2(at.x + d, at.y + d), Alpha(ink.rim, 0.8f), text, nullptr, wrap);
    dl->AddText(fonts.face, size, at, color, text, nullptr, wrap);
}

// Chrome type as the game's menus draw it: a dark rim and drop shadow, the face thickened by a
// second pass, its upper and lower halves shaded apart at the capitals' middle.
inline void Metal(ImDrawList* dl, const Ink& ink, const Fonts& fonts, float size, ImVec2 at, const char* text,
                  const Chrome& chrome) {
    ImFont* font = fonts.face;
    ImFontBaked* baked = font->GetFontBaked(size);
    const ImFontGlyph* cap = baked->FindGlyph('H');
    const float scale = size / baked->Size;
    const float cap_top = at.y + (cap ? cap->Y0 * scale : size * 0.2f);
    const float cap_bottom = at.y + (cap ? cap->Y1 * scale : size * 0.85f);
    const float horizon = cap_top + (cap_bottom - cap_top) * 0.52f;
    const float rim = std::max(1.0f, size * 0.055f), weight = std::max(0.75f, size * 0.04f);

    dl->AddText(font, size, ImVec2(at.x + rim * 1.6f, at.y + rim * 2.2f), Alpha(ink.rim, 0.55f), text);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx || dy) dl->AddText(font, size, ImVec2(at.x + dx * rim, at.y + dy * rim), ink.rim, text);
        }
    }
    dl->AddText(font, size, ImVec2(at.x + weight + rim, at.y), ink.rim, text);
    const ImVec4 halves[2] = {ImVec4(-FLT_MAX, -FLT_MAX, FLT_MAX, horizon), ImVec4(-FLT_MAX, horizon, FLT_MAX, FLT_MAX)};
    for (int half = 0; half < 2; ++half) {
        const int first = dl->VtxBuffer.Size;
        for (float x : {at.x, at.x + weight}) dl->AddText(font, size, ImVec2(x, at.y), IM_COL32_WHITE, text, nullptr, 0.0f, &halves[half]);
        const bool upper = half == 0;
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, first, dl->VtxBuffer.Size,
                                                      ImVec2(at.x, upper ? cap_top : horizon),
                                                      ImVec2(at.x, upper ? horizon : cap_bottom),
                                                      upper ? chrome.top : chrome.low,
                                                      upper ? chrome.horizon : chrome.bottom);
    }
}

// An arrow ("<" for dir -1, ">" for 1) as one stroke, centred on `at`.
inline void Chevron(ImDrawList* dl, ImVec2 at, float size, int dir, ImU32 color, float thickness) {
    const float w = size * 0.28f * float(dir), h = size * 0.5f;
    const ImVec2 points[3] = {ImVec2(at.x - w, at.y - h), ImVec2(at.x + w, at.y), ImVec2(at.x - w, at.y + h)};
    dl->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
}

// A soft round glow: rings of fading colour.
inline void Glow(ImDrawList* dl, ImVec2 c, float r, ImU32 color, int steps = 10) {
    for (int i = steps; i >= 1; --i) {
        const float t = float(i) / float(steps);
        dl->AddCircleFilled(c, r * t, Alpha(color, 0.11f * (1.0f - t) + 0.02f), 32);
    }
}

// The menu cursor: a glowing purple orb, then a cyan arrowhead pointing at the item. `tip` is
// where the arrow points (the item's left edge, on its middle).
inline void Cursor(ImDrawList* dl, const Ink& ink, ImVec2 tip, float px) {
    const float r = 15.0f * px;
    const ImVec2 orb(tip.x - 44.0f * px, tip.y);
    Glow(dl, orb, r * 2.4f, ink.orb);
    dl->AddCircleFilled(orb, r, Alpha(ink.orb, 0.85f), 32);
    dl->AddCircle(orb, r, Alpha(ink.orb_core, 0.7f), 32, std::max(1.0f, 2.0f * px));
    dl->AddCircleFilled(orb, r * 0.5f, ink.orb_core, 24);
    const float a = 11.0f * px;
    dl->AddTriangleFilled(ImVec2(tip.x - 4.0f * px, tip.y), ImVec2(tip.x - 4.0f * px - a * 1.1f, tip.y - a),
                          ImVec2(tip.x - 4.0f * px - a * 1.1f, tip.y + a), ink.accent);
}

// Deep space for settings.exe, which has no game behind it: a blue-grey sky, two faint nebulae
// and a fixed scatter of stars (the same on every frame and size).
inline void Starfield(ImDrawList* dl, const Ink& ink, ImVec2 o, ImVec2 size) {
    const ImVec2 end(o.x + size.x, o.y + size.y);
    dl->AddRectFilledMultiColor(o, end, ink.space_top, ink.space_top, ink.space_bottom, ink.space_bottom);
    const float px = size.y / 1080.0f;
    for (int i = 10; i >= 1; --i) {
        dl->AddCircleFilled(ImVec2(o.x + size.x * 0.28f, o.y + size.y * 0.62f), size.y * 0.07f * float(i), ink.nebula_a, 64);
        dl->AddCircleFilled(ImVec2(o.x + size.x * 0.78f, o.y + size.y * 0.30f), size.y * 0.06f * float(i), ink.nebula_b, 64);
    }
    uint32_t seed = 0x5d3a9e1u;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24);
    };
    for (int i = 0; i < 700; ++i) {
        const ImVec2 at(o.x + next() * size.x, o.y + next() * size.y);
        const float bright = next(), r = (0.5f + bright * bright * 1.6f) * px;
        dl->AddCircleFilled(at, std::max(0.6f, r), Alpha(ink.star, 0.25f + 0.75f * bright), 8);
        if (bright > 0.97f) Glow(dl, at, r * 7.0f, ink.star, 5);
    }
}

// The screen behind a page: the game under a dark veil (deepest at the top and bottom), or deep
// space when `space` (nothing behind).
inline void Backdrop(ImDrawList* dl, const Ink& ink, ImVec2 o, ImVec2 size, bool space = false) {
    if (space) Starfield(dl, ink, o, size);
    const float mid = o.y + size.y * 0.5f, right = o.x + size.x, bottom = o.y + size.y;
    dl->AddRectFilledMultiColor(o, ImVec2(right, mid), ink.veil_edge, ink.veil_edge, ink.veil_mid, ink.veil_mid);
    dl->AddRectFilledMultiColor(ImVec2(o.x, mid), ImVec2(right, bottom), ink.veil_mid, ink.veil_mid, ink.veil_edge,
                                ink.veil_edge);
}

// The page title in large chrome gold, centred near the top.
inline void Title(ImDrawList* dl, const Ink& ink, const Fonts& fonts, ImVec2 o, ImVec2 size, float px, const char* title) {
    float title_size = 64.0f * px;
    const float tw = Width(fonts, title_size, title), room = size.x - 120.0f * px;
    if (tw > room) title_size *= room / tw;
    const float w = Width(fonts, title_size, title);
    Metal(dl, ink, fonts, title_size, ImVec2(o.x + (size.x - w) * 0.5f, o.y + 52.0f * px), title, ink.gold);
}

// A frame as the game's: a pale blue line with rounded corners and a soft halo, over dark glass.
inline void Panel(ImDrawList* dl, const Ink& ink, ImVec2 a, ImVec2 b, float px) {
    const float round = 16.0f * px, line = std::max(1.0f, 2.0f * px);
    const int first = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, ink.panel_top, round);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, first, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), ink.panel_top,
                                                  ink.panel_bottom);
    for (int i = 3; i >= 1; --i) {
        const float g = float(i) * 2.5f * px;
        dl->AddRect(ImVec2(a.x - g * 0.5f, a.y - g * 0.5f), ImVec2(b.x + g * 0.5f, b.y + g * 0.5f),
                    Alpha(ink.frame_glow, 1.0f / float(i)), round + g * 0.5f, 0, line + g);
    }
    dl->AddRect(a, b, ink.frame, round, 0, line);
    const float inset = 5.0f * px;
    dl->AddRect(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), Alpha(ink.frame, 0.22f),
                std::max(0.0f, round - inset), 0, std::max(1.0f, px));
}

// A menu item: chrome gold capitals, brighter when focused with the cursor before them, steel when
// it does not apply. `at` is the text's top left.
inline void MenuItem(ImDrawList* dl, const Ink& ink, const Fonts& fonts, float size, ImVec2 at, const char* text,
                     bool focus, bool enabled, float px) {
    const std::string caps = Upper(text);
    if (focus) Cursor(dl, ink, ImVec2(at.x - 6.0f * px, at.y + size * 0.52f), px);
    Metal(dl, ink, fonts, size, at, caps.c_str(), !enabled ? ink.steel : focus ? ink.gold : ink.gold_dim);
}

// A pad glyph as the game's button hints: a chrome ring around a dark face with the button's mark
// in its colour; the d-pad as a pale cross.
enum class Glyph { Cross, Circle, Triangle, UpDown };
inline void PadGlyph(ImDrawList* dl, const Ink& ink, ImVec2 c, float r, Glyph glyph, float px) {
    const float line = std::max(1.0f, 2.6f * px);
    if (glyph == Glyph::UpDown) {
        const float arm = r * 0.36f, len = r * 1.05f;
        auto plus = [&](float grow, ImU32 color) {
            dl->AddRectFilled(ImVec2(c.x - arm - grow, c.y - len - grow), ImVec2(c.x + arm + grow, c.y + len + grow), color,
                              2.0f * px);
            dl->AddRectFilled(ImVec2(c.x - len - grow, c.y - arm - grow), ImVec2(c.x + len + grow, c.y + arm + grow), color,
                              2.0f * px);
        };
        plus(2.5f * px, ink.rim);
        plus(0.0f, ink.ring);
        const float m = arm * 0.8f;
        dl->AddTriangleFilled(ImVec2(c.x, c.y - len + 2.0f * px), ImVec2(c.x - m, c.y - len + m + 3.0f * px),
                              ImVec2(c.x + m, c.y - len + m + 3.0f * px), ink.accent);
        dl->AddTriangleFilled(ImVec2(c.x, c.y + len - 2.0f * px), ImVec2(c.x + m, c.y + len - m - 3.0f * px),
                              ImVec2(c.x - m, c.y + len - m - 3.0f * px), ink.accent);
        return;
    }
    dl->AddCircleFilled(c, r + 2.0f * px, ink.rim, 32);
    dl->AddCircleFilled(c, r, ink.ring, 32);
    dl->AddCircleFilled(c, r * 0.78f, ink.face, 32);
    dl->PathArcTo(c, r * 0.66f, IM_PI * 1.15f, IM_PI * 1.85f, 12);
    dl->PathStroke(Alpha(ink.ring, 0.35f), ImDrawFlags_None, std::max(1.0f, 1.5f * px));
    const float m = r * 0.36f;
    switch (glyph) {
    case Glyph::Cross:
        dl->AddLine(ImVec2(c.x - m, c.y - m), ImVec2(c.x + m, c.y + m), ink.cross, line);
        dl->AddLine(ImVec2(c.x - m, c.y + m), ImVec2(c.x + m, c.y - m), ink.cross, line);
        break;
    case Glyph::Circle: dl->AddCircle(c, r * 0.4f, ink.circle, 24, line); break;
    case Glyph::Triangle:
        dl->AddTriangle(ImVec2(c.x, c.y - m * 1.1f), ImVec2(c.x + m * 1.05f, c.y + m * 0.75f),
                        ImVec2(c.x - m * 1.05f, c.y + m * 0.75f), ink.triangle, line);
        break;
    case Glyph::UpDown: break;
    }
}

struct Hint {
    Glyph glyph;
    const char* label;
};
// The top of the hint strip along the bottom.
inline float HintsTop(ImVec2 o, ImVec2 size, float px) { return o.y + size.y - 84.0f * px; }

// A small line of notes at the bottom left, under the keyboard keys (line 0 is the keys').
inline void Note(ImDrawList* dl, const Ink& ink, const Fonts& fonts, ImVec2 o, ImVec2 size, float px, int line,
                 const char* text, ImU32 color) {
    const float s = 17.0f * px;
    Text(dl, ink, fonts, s, ImVec2(o.x + 48.0f * px, HintsTop(o, size, px) + 8.0f * px + float(line) * (s + 10.0f * px)),
         text, color);
}

// The button hints along the bottom right, as the game's, and the keyboard's keys for the same at
// the bottom left. Each hint's rectangle goes to `rects` when given (settings.exe clicks them).
inline void Hints(ImDrawList* dl, const Ink& ink, const Fonts& fonts, ImVec2 o, ImVec2 size, float px, const Hint* hints,
                  int count, const char* keys, std::vector<ImRect>* rects = nullptr) {
    const float text = 28.0f * px, r = 19.0f * px, gap = 12.0f * px, space = 40.0f * px, y = HintsTop(o, size, px);
    std::vector<std::string> labels;
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        labels.push_back(Upper(hints[i].label));
        total += r * 2.0f + gap + Width(fonts, text, labels.back().c_str()) + (i ? space : 0.0f);
    }
    float x = o.x + size.x - 60.0f * px - total;
    if (rects) rects->clear();
    for (int i = 0; i < count; ++i) {
        const float x0 = x, mid = y + text * 0.55f;
        PadGlyph(dl, ink, ImVec2(x + r, mid), r, hints[i].glyph, px);
        x += r * 2.0f + gap;
        Metal(dl, ink, fonts, text, ImVec2(x, y), labels[i].c_str(), ink.silver);
        x += Width(fonts, text, labels[i].c_str());
        if (rects) rects->emplace_back(ImVec2(x0, y - 8.0f * px), ImVec2(x, y + text + 8.0f * px));
        x += space;
    }
    Note(dl, ink, fonts, o, size, px, 0, keys, ink.text_dim);
}

// One row of a settings page.
struct Row {
    std::string label, value, help;
    bool changes = true; ///< Left/Right change it (arrows around the value)
    bool enabled = true; ///< applies in the current window mode; else greyed out and skipped
    int check = -1;      ///< a checkbox instead of the value: 0 clear, 1 ticked (the Cheats page)
};

// A checkbox as the game's frames: a rounded square in the glowing pale blue line, ticked with a
// gold check. `c` is its centre.
inline void CheckBox(ImDrawList* dl, const Ink& ink, ImVec2 c, float half, bool ticked, bool focus, float px) {
    const ImVec2 a(c.x - half, c.y - half), b(c.x + half, c.y + half);
    const float round = 5.0f * px;
    if (focus) Glow(dl, c, half * 1.9f, Alpha(ink.frame_glow, 0.35f), 8);
    dl->AddRectFilled(a, b, ink.panel_bottom, round);
    dl->AddRect(ImVec2(a.x - 2.0f * px, a.y - 2.0f * px), ImVec2(b.x + 2.0f * px, b.y + 2.0f * px), Alpha(ink.frame_glow, 0.5f),
                round + 2.0f * px, 0, std::max(1.0f, 3.0f * px));
    dl->AddRect(a, b, ink.frame, round, 0, std::max(1.0f, 2.2f * px));
    if (!ticked) return;
    const ImVec2 p0(c.x - half * 0.55f, c.y + half * 0.02f), p1(c.x - half * 0.15f, c.y + half * 0.45f),
        p2(c.x + half * 0.6f, c.y - half * 0.5f);
    const float w = std::max(2.0f, 5.5f * px);
    dl->AddPolyline(std::array<ImVec2, 3>{p0, p1, p2}.data(), 3, ink.rim, ImDrawFlags_None, w + 3.0f * px);
    dl->AddPolyline(std::array<ImVec2, 3>{p0, p1, p2}.data(), 3, ink.gold.horizon, ImDrawFlags_None, w);
}
// Where a row is on screen: its line, and the "<" arrow's span (a click there steps back).
struct RowArea {
    ImRect bar;
    float back_x0 = 0.0f, back_x1 = 0.0f;
};

// A settings page: the backdrop and title, a framed panel listing the rows (name on the left in
// gold, value on the right in silver, the focused one with the cursor, a faint cyan band and arrows
// around its value), an info panel with the focused row's help, and the button hints. `space` draws
// the starfield behind (no game). `areas` and `hint_rects` receive the clickable spans.
inline void SettingsPage(ImDrawList* dl, const Ink& ink, const Fonts& fonts, ImVec2 o, ImVec2 size, const char* title,
                         const std::vector<Row>& rows, int focus_row, const Hint* hints, int hint_count,
                         const char* keys, std::vector<RowArea>* areas = nullptr,
                         std::vector<ImRect>* hint_rects = nullptr, bool space = false) {
    const float px = size.y / 1080.0f;
    const int count = int(rows.size());
    Backdrop(dl, ink, o, size, space);
    Title(dl, ink, fonts, o, size, px, title);

    // The list, in a frame on the left; the info frame beside it.
    const float row_h = 62.0f * px, pad = 26.0f * px;
    const float list_w = std::min(size.x * 0.60f, 1150.0f * px), info_w = std::min(size.x * 0.27f, 520.0f * px);
    const float space_w = 36.0f * px, total_w = list_w + space_w + info_w;
    const float x0 = o.x + (size.x - total_w) * 0.5f, x1 = x0 + list_w;
    const float panel_h = pad * 2.0f + float(count) * row_h;
    const float top = o.y + std::max(160.0f * px, (size.y - panel_h) * 0.5f);
    Panel(dl, ink, ImVec2(x0, top), ImVec2(x1, top + panel_h), px);

    const float label = 30.0f * px, value_size = 24.0f * px;
    const float lx = x0 + 96.0f * px, vx1 = x1 - 56.0f * px;
    float y = top + pad;
    const Row* focused = nullptr;
    if (areas) areas->assign(rows.size(), RowArea{});
    for (int row = 0; row < count; ++row) {
        const Row& r = rows[row];
        const bool focus = row == focus_row;
        const float mid = y + row_h * 0.5f;
        const ImRect bar(ImVec2(x0 + 8.0f * px, y + 4.0f * px), ImVec2(x1 - 8.0f * px, y + row_h - 4.0f * px));
        if (focus) {
            dl->AddRectFilledMultiColor(bar.Min, ImVec2((bar.Min.x + bar.Max.x) * 0.5f, bar.Max.y), Alpha(ink.band, 0.0f),
                                        ink.band, ink.band, Alpha(ink.band, 0.0f));
            dl->AddRectFilledMultiColor(ImVec2((bar.Min.x + bar.Max.x) * 0.5f, bar.Min.y), bar.Max, ink.band,
                                        Alpha(ink.band, 0.0f), Alpha(ink.band, 0.0f), ink.band);
        }
        MenuItem(dl, ink, fonts, label, ImVec2(lx, mid - label * 0.55f), r.label.c_str(), focus, r.enabled, px);

        if (r.check >= 0) { // a checkbox at the right, where the value would be
            const float half = row_h * 0.26f;
            CheckBox(dl, ink, ImVec2(vx1 - half, mid), half, r.check > 0, focus, px);
            if (areas) (*areas)[row] = {bar, 0.0f, 0.0f};
            if (focus) focused = &r;
            y += row_h;
            continue;
        }
        // The value, right-aligned, shrunk to fit beside the name.
        const std::string value = Upper(r.value);
        const float name_end = lx + Width(fonts, label, Upper(r.label).c_str()) + 48.0f * px;
        float vs = value_size;
        const float room = vx1 - name_end, vw0 = Width(fonts, vs, value.c_str());
        if (vw0 > room && room > 0.0f) vs = std::max(value_size * 0.6f, vs * room / vw0);
        const float vw = Width(fonts, vs, value.c_str()), value_x = vx1 - vw;
        Metal(dl, ink, fonts, vs, ImVec2(value_x, mid - vs * 0.55f), value.c_str(), r.enabled ? ink.silver : ink.steel);
        if (focus && r.changes) {
            const float chevron = row_h * 0.34f, stroke = 3.0f * px;
            Chevron(dl, ImVec2(value_x - 22.0f * px, mid), chevron, -1, ink.accent, stroke);
            Chevron(dl, ImVec2(vx1 + 24.0f * px, mid), chevron, 1, ink.accent, stroke);
        }
        if (areas) (*areas)[row] = {bar, value_x - 42.0f * px, value_x - 4.0f * px};
        if (focus) focused = &r;
        y += row_h;
    }

    // The info frame: the focused row's name in gold, a cyan heading, then its help.
    const float ix0 = x1 + space_w, ix1 = ix0 + info_w, ipad = 28.0f * px;
    Panel(dl, ink, ImVec2(ix0, top), ImVec2(ix1, top + panel_h), px);
    if (focused) {
        const std::string heading = Upper(focused->label);
        float head = 28.0f * px;
        const float hw = Width(fonts, head, heading.c_str()), room = info_w - ipad * 2.0f;
        if (hw > room) head *= room / hw;
        const float sub = 18.0f * px, body = 21.0f * px;
        float iy = top + ipad;
        Metal(dl, ink, fonts, head, ImVec2(ix0 + ipad, iy), heading.c_str(), ink.gold);
        iy += head + 16.0f * px;
        Text(dl, ink, fonts, sub, ImVec2(ix0 + ipad, iy), "SETTING INFO", ink.accent);
        iy += sub + 12.0f * px;
        dl->AddLine(ImVec2(ix0 + ipad, iy), ImVec2(ix1 - ipad, iy), Alpha(ink.frame, 0.35f), std::max(1.0f, px));
        iy += 16.0f * px;
        const std::string help = Upper(focused->help);
        dl->PushClipRect(ImVec2(ix0, top), ImVec2(ix1, top + panel_h - 12.0f * px), true);
        Text(dl, ink, fonts, body, ImVec2(ix0 + ipad, iy), help.c_str(), ink.text, room);
        dl->PopClipRect();
    }
    Hints(dl, ink, fonts, o, size, px, hints, hint_count, keys, hint_rects);
}

} // namespace StardustPage
