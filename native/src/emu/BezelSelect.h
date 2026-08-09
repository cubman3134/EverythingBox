// Pure, header-only bezel selection + viewport parsing for the emulator's decoration layer (issue #106).
//
// Three units, all free of Qt and free of I/O so they can be pinned by probe_bezel without a window, a
// QCoreApplication or a scratch dir (mirrors StateSlots.h). RetroView glues them to disk and to QPainter.
//
//   candidates()   — the SELECTION PRECEDENCE for a session, as an ordered list of relative PNG paths.
//   parseViewport()— the community info-file (RetroArch/RetroBat .cfg/.info) -> a screen-viewport rect.
//   mapViewport()  — a viewport (in the bezel image's native pixels) -> the game's on-screen dst rect,
//                    letterboxing the bezel so the art never distorts on a non-16:9 window.
//
// The viewport path is ADDITIVE: an empty parseViewport() (no info file, or one carrying no viewport)
// leaves RetroView on exactly its prior flat-overlay behaviour. Nothing here forces the new path.
#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <cmath>
#include <algorithm>

namespace BezelSelect {

// A screen viewport in the bezel image's own native pixel coordinates. `valid` is false until an info file
// supplies a complete, sane rectangle — the viewport path is gated on it.
struct Viewport
{
    int  x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
};

// The game's on-screen destination (gx,gy,gw,gh) and the bezel art's letterboxed destination
// (bx,by,bw,bh), both in widget pixel coordinates. Same scale factor for both, so the game lands exactly in
// the bezel's cutout however the window is shaped.
struct Mapped
{
    int gx = 0, gy = 0, gw = 0, gh = 0;
    int bx = 0, by = 0, bw = 0, bh = 0;
};

// Precedence-ordered candidate PNG paths (relative to the <data>/bezels directory) for one session:
//
//   <system>/<rom>.png    game-specific  (only when BOTH system and rom are non-empty)
//   <system>/default.png  per-system     (only when system is non-empty)
//   <core>.png            global, legacy (only when core is non-empty) — today's behaviour
//   default.png           global, legacy                              — today's behaviour
//
// The two legacy tiers preserve exactly what the folder did before #106, so a user who already dropped
// fceumm.png / default.png in bezels/ keeps their setup. The caller picks the first that exists on disk.
inline std::vector<std::string> candidates(const std::string& system,
                                           const std::string& rom,
                                           const std::string& core)
{
    std::vector<std::string> out;
    if (!system.empty() && !rom.empty()) out.push_back(system + "/" + rom + ".png");
    if (!system.empty())                 out.push_back(system + "/default.png");
    if (!core.empty())                   out.push_back(core + ".png");
    out.push_back("default.png");
    return out;
}

namespace detail {

// Trim ASCII whitespace and one layer of surrounding double quotes from a value.
inline std::string clean(std::string s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    s = s.substr(a, b - a);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

inline std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Parse a decimal integer prefix ("1494", "213 "), returning false on no digits at all. Defensive: a value
// like "auto" or "" fails rather than silently reading 0, so a malformed info file leaves the viewport
// invalid and RetroView stays flat.
inline bool toInt(const std::string& v, int& out)
{
    size_t i = 0;
    bool neg = false;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) { neg = (v[i] == '-'); ++i; }
    if (i >= v.size() || !std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    long n = 0;
    for (; i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])); ++i) n = n * 10 + (v[i] - '0');
    out = static_cast<int>(neg ? -n : n);
    return true;
}

} // namespace detail

// Parse a RetroArch/RetroBat-style info file for the screen viewport. Lines are `key = value`; `#` and `;`
// start comments; keys are matched case-insensitively; values may be quoted. Recognised:
//
//   custom_viewport_width   custom_viewport_height   custom_viewport_x   custom_viewport_y
//
// All four must be present and the rectangle sane (w>0, h>0, x>=0, y>=0) for `valid` to be true — an
// all-or-nothing parse, because a half-specified viewport would place the game wrong, which is worse than
// the flat fallback. Unknown keys (the rest of a real RetroArch .cfg) are ignored.
inline Viewport parseViewport(const std::string& text)
{
    bool hasW = false, hasH = false, hasX = false, hasY = false;
    int  w = 0, h = 0, x = 0, y = 0;

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (nl == std::string::npos) pos = text.size() + 1; else pos = nl + 1;

        // Strip a trailing CR (CRLF files) and anything from a comment marker.
        for (char marker : { '#', ';' })
        {
            size_t c = line.find(marker);
            if (c != std::string::npos) line = line.substr(0, c);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = detail::lower(detail::clean(line.substr(0, eq)));
        std::string val = detail::clean(line.substr(eq + 1));

        int n = 0;
        if      (key == "custom_viewport_width")  { if (detail::toInt(val, n)) { w = n; hasW = true; } }
        else if (key == "custom_viewport_height") { if (detail::toInt(val, n)) { h = n; hasH = true; } }
        else if (key == "custom_viewport_x")      { if (detail::toInt(val, n)) { x = n; hasX = true; } }
        else if (key == "custom_viewport_y")      { if (detail::toInt(val, n)) { y = n; hasY = true; } }
    }

    Viewport vp;
    if (hasW && hasH && hasX && hasY && w > 0 && h > 0 && x >= 0 && y >= 0)
    {
        vp.x = x; vp.y = y; vp.w = w; vp.h = h; vp.valid = true;
    }
    return vp;
}

// Map a viewport (bezel-native pixels) into a widget of size (widgetW, widgetH). The bezel is fitted into
// the widget preserving ITS aspect ratio (KeepAspectRatio -> letterbox/pillarbox on a mismatched window,
// per the issue's aspect note), and the viewport is scaled by that same factor and offset into the fitted
// bezel, so the emulated picture lands in the cutout with no art distortion.
inline Mapped mapViewport(int bezelW, int bezelH, int widgetW, int widgetH, const Viewport& vp)
{
    Mapped m;
    if (bezelW <= 0 || bezelH <= 0 || widgetW <= 0 || widgetH <= 0) return m;

    const double scale = std::min(static_cast<double>(widgetW) / bezelW,
                                  static_cast<double>(widgetH) / bezelH);
    const int bw = static_cast<int>(std::lround(bezelW * scale));
    const int bh = static_cast<int>(std::lround(bezelH * scale));
    const int bx = (widgetW - bw) / 2;
    const int by = (widgetH - bh) / 2;

    m.bx = bx; m.by = by; m.bw = bw; m.bh = bh;
    m.gx = bx + static_cast<int>(std::lround(vp.x * scale));
    m.gy = by + static_cast<int>(std::lround(vp.y * scale));
    m.gw = static_cast<int>(std::lround(vp.w * scale));
    m.gh = static_cast<int>(std::lround(vp.h * scale));
    return m;
}

} // namespace BezelSelect
