/* PS5 RetroArch - the remote core's screens: games on download sources.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The remote core (cores/remote/) runs a stub (src/remote/library.h) and shows what it
 * stands for, drawn by the title (the core only hands frames and buttons through,
 * src/remote/remote_core.h):
 *
 *   - a game on the sources: the download dialog. With several sources having the game,
 *     the source dialog asks which one first. Cross on the game downloads it (first in the
 *     queue) and starts it once it is all there, in this RetroArch, with its platform's
 *     core (the remote core goes; the game is in its system's list then). Circle keeps
 *     downloading in the background, Square cancels;
 *   - Downloads: the sources (Cross reads their lists again), the download queue (Cross
 *     tries a failed one again, Square cancels) and the games downloaded from them (Square
 *     twice deletes one from the console; its saves stay). Circle goes back.
 */
#pragma once

#include "../library.h"
#include "canvas.h"
#include "services.h"

#include <cstdint>
#include <memory>

namespace ps5::remote::ui
{
/* The buttons, as the remote core reads RetroArch's pad. */
enum Button : uint32_t
{
    up = 1u << 0,
    down = 1u << 1,
    left = 1u << 2,
    right = 1u << 3,
    cross = 1u << 4,
    circle = 1u << 5,
    square = 1u << 6,
    triangle = 1u << 7,
};

class Screen
{
  public:
    virtual ~Screen() = default;
    /* The buttons pressed since the last frame. */
    virtual void input(uint32_t pressed) = 0;
    virtual void draw(Canvas &canvas) = 0;
    /* The core closes its content: the screen is done. */
    virtual bool finished() const = 0;
};

/* The screen of a stub. */
std::unique_ptr<Screen> open(const Stub &stub, Services &services);
} // namespace ps5::remote::ui
