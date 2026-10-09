/* PS5 RetroArch - the remote core's screens drawn into its frame (src/remote/ui/canvas.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "canvas.h"

#include "../remote.h"

#include "gfx/drivers_font_renderer/bitmap.h"

#include <algorithm>
#include <vector>

namespace ps5::remote::ui
{
void Canvas::fill(int x, int y, int width, int height, uint32_t color)
{
    const int left = std::max(x, 0), top = std::max(y, 0);
    const int right = std::min(x + width, int(width_)), bottom = std::min(y + height, int(height_));
    for (int row = top; row < bottom; row++)
        std::fill(pixels_ + size_t(row) * width_ + left, pixels_ + size_t(row) * width_ + right,
                  color);
}

void Canvas::text(int x, int y, const std::string &text_, uint32_t color, int scale, int width)
{
    std::string line = plain_text(text_);
    const int room = (width > 0 ? width : int(width_) - x) / advance(scale);
    if (room <= 0)
        return;
    if (int(line.size()) > room)
        line = line.substr(0, size_t(std::max(room - 3, 0))) + "...";
    for (size_t i = 0; i < line.size(); i++)
    {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const int left = x + int(i) * advance(scale);
        for (int gy = 0; gy < FONT_HEIGHT; gy++)
            for (int gx = 0; gx < FONT_WIDTH; gx++)
            {
                const unsigned bit = unsigned(gx + gy * FONT_WIDTH);
                if (bitmap_bin[FONT_OFFSET(c) + (bit >> 3)] & (1u << (bit & 7)))
                    fill(left + gx * scale, y + gy * scale, scale, scale, color);
            }
    }
}

void Canvas::centred(int x, int y, const std::string &text_, uint32_t color, int scale)
{
    const int length = int(plain_text(text_).size()) * advance(scale);
    text(std::max(0, x - length / 2), y, text_, color, scale);
}

int Canvas::block(int x, int y, const std::string &text_, uint32_t color, int width, int scale)
{
    const size_t room = size_t(std::max(width / advance(scale), 1));
    const std::string all = plain_text(text_);
    size_t at = 0;
    while (at < all.size())
    {
        size_t end = std::min(all.size(), at + room);
        if (end < all.size())
        {
            const size_t space = all.rfind(' ', end);
            if (space != std::string::npos && space > at)
                end = space;
        }
        text(x, y, all.substr(at, end - at), color, scale, width);
        y += line(scale);
        at = end;
        while (at < all.size() && all[at] == ' ')
            at++;
    }
    return y;
}

void Canvas::bar(int x, int y, int width, int height, double share, uint32_t color)
{
    fill(x, y, width, height, color::track);
    fill(x, y, int(width * std::clamp(share, 0.0, 1.0)), height, color);
}
} // namespace ps5::remote::ui
