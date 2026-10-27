// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - the shield icon, rendered in code so it needs no image files
// and no icon theme: the Linux tray icons (StatusNotifierItem and XEmbed) and
// the generated Windows .ico files all come from here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fg {
namespace icon {

enum class Style { Active, Paused, DryRun };

// Half-width of the shield outline at height y (both in [-1, 1], y down);
// negative outside. Flat top with rounded corners, tapering to a point.
inline float shield_halfwidth(float y) {
    constexpr float top = -0.86f, waist = 0.0f, bottom = 0.94f, w = 0.74f, r = 0.2f;
    if (y < top || y > bottom) return -1.0f;
    if (y < top + r) {
        const float dy = top + r - y;
        return w - r + std::sqrt(std::max(0.0f, r * r - dy * dy));
    }
    if (y <= waist) return w;
    const float t = (y - waist) / (bottom - waist);
    return w * (1.0f - t * t);
}

// The shield scaled by k around its centre.
inline bool in_shield(float x, float y, float k) {
    constexpr float cy = 0.04f;
    return std::fabs(x / k) <= shield_halfwidth((y - cy) / k + cy);
}

inline float segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

inline bool in_glyph(Style st, float x, float y) {
    switch (st) {
        case Style::Active:  // check mark
            return segment_distance(x, y, -0.36f, 0.02f, -0.1f, 0.3f) < 0.1f ||
                   segment_distance(x, y, -0.1f, 0.3f, 0.36f, -0.3f) < 0.1f;
        case Style::Paused:  // pause bars
            return std::fabs(x) >= 0.09f && std::fabs(x) <= 0.29f && y >= -0.38f && y <= 0.34f;
        case Style::DryRun:
            return false;
    }
    return false;
}

// size x size pixels, row by row, as 0xAARRGGBB with straight (not
// premultiplied) alpha. 4x4 supersampling gives smooth edges.
inline std::vector<uint32_t> render(Style st, int size) {
    struct Rgb {
        float r, g, b;
    };
    Rgb border{0.11f, 0.31f, 0.54f}, light{0.29f, 0.60f, 0.94f}, dark{0.18f, 0.45f, 0.82f};
    if (st == Style::Paused) {
        border = {0.33f, 0.33f, 0.33f}, light = {0.66f, 0.66f, 0.66f}, dark = {0.54f, 0.54f, 0.54f};
    } else if (st == Style::DryRun) {
        border = {0.55f, 0.36f, 0.0f}, light = {1.0f, 0.77f, 0.25f}, dark = {0.91f, 0.64f, 0.10f};
    }
    const Rgb white{1.0f, 1.0f, 1.0f};
    constexpr int kSub = 4;

    std::vector<uint32_t> out((size_t)size * size);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            float a = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const float x = (px + (sx + 0.5f) / kSub) / size * 2 - 1;
                    const float y = (py + (sy + 0.5f) / kSub) / size * 2 - 1;
                    const Rgb* c = nullptr;
                    if (in_shield(x, y, 0.8f)) c = in_glyph(st, x, y) ? &white : x < 0 ? &light : &dark;
                    else if (in_shield(x, y, 1.0f)) c = &border;
                    if (!c) continue;
                    a += 1, r += c->r, g += c->g, b += c->b;
                }
            }
            if (a == 0) continue;
            auto byte = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
            out[(size_t)py * size + px] =
                byte(a / (kSub * kSub)) << 24 | byte(r / a) << 16 | byte(g / a) << 8 | byte(b / a);
        }
    }
    return out;
}

}  // namespace icon
}  // namespace fg
