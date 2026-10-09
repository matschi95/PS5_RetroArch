/* PS5 RetroArch - the remote core's screens drawn into its frame: rectangles, bars and text
 * in RetroArch's own bitmap font (RGUI's, vendor/retroarch/gfx/drivers_font_renderer).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>

namespace ps5::remote::ui
{
/* The frame the remote core hands RetroArch: XRGB8888. */
constexpr unsigned frame_width = 960;
constexpr unsigned frame_height = 540;

namespace color
{
constexpr uint32_t background = 0x101418;
constexpr uint32_t panel = 0x1a2029;
constexpr uint32_t focus = 0x24364d;
constexpr uint32_t title = 0xf2f2f2;
constexpr uint32_t text = 0xd8dbe0;
constexpr uint32_t meta = 0x8a9099;
constexpr uint32_t accent = 0x2f80ed;
constexpr uint32_t track = 0x2a2f38;
constexpr uint32_t good = 0x6fcf97;
constexpr uint32_t warning = 0xeb5757;
} // namespace color

class Canvas
{
  public:
    explicit Canvas(uint32_t *pixels, unsigned width = frame_width, unsigned height = frame_height)
        : pixels_(pixels), width_(width), height_(height)
    {
    }
    unsigned width() const
    {
        return width_;
    }
    unsigned height() const
    {
        return height_;
    }
    void fill(int x, int y, int width, int height, uint32_t color);
    /* One line of text at a scale (1: 6 pixels a character, 11 a line), its accents taken
     * off (plain_text), cut with "..." to at most `width` pixels (0: the frame's right). */
    void text(int x, int y, const std::string &text, uint32_t color, int scale = 2, int width = 0);
    /* The same, centred on x. */
    void centred(int x, int y, const std::string &text, uint32_t color, int scale = 2);
    /* Lines of text broken at spaces to `width` pixels; the y below the last. */
    int block(int x, int y, const std::string &text, uint32_t color, int width, int scale = 2);
    /* A bar `share` (0..1) full. */
    void bar(int x, int y, int width, int height, double share, uint32_t color = color::accent);
    /* A character's width and a line's height at a scale. */
    static int advance(int scale)
    {
        return 6 * scale;
    }
    static int line(int scale)
    {
        return 11 * scale;
    }

  private:
    uint32_t *pixels_;
    unsigned width_, height_;
};
} // namespace ps5::remote::ui
