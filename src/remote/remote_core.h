/* PS5 RetroArch - what the remote core calls in eboot.bin.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The remote core (cores/remote/remote_libretro.c, PS5_LIBRARY_FETCH_CORE) runs the stubs
 * of the download sources' games and screens (src/remote/library.h). Everything it shows is
 * eboot.bin's (src/remote/ui/remote_ui.h): a core's imports are bound to the title's
 * functions by name (tools/core-imports.py), so these are what it imports, and it only
 * hands frames and buttons through.
 */
#ifndef PS5_RETROARCH_REMOTE_CORE_H
#define PS5_RETROARCH_REMOTE_CORE_H

#include <stdint.h>

#define PS5_REMOTE_FRAME_WIDTH 960
#define PS5_REMOTE_FRAME_HEIGHT 540

/* The buttons pressed since the last frame (src/remote/ui/remote_ui.h's). */
#define PS5_REMOTE_UP (1u << 0)
#define PS5_REMOTE_DOWN (1u << 1)
#define PS5_REMOTE_LEFT (1u << 2)
#define PS5_REMOTE_RIGHT (1u << 3)
#define PS5_REMOTE_CROSS (1u << 4)
#define PS5_REMOTE_CIRCLE (1u << 5)
#define PS5_REMOTE_SQUARE (1u << 6)
#define PS5_REMOTE_TRIANGLE (1u << 7)

#ifdef __cplusplus
extern "C"
{
#endif

    /* The core's content (a stub) is loaded: its screen opens. 0, or -1 when it is no stub. */
    int ps5_remote_core_open(const char *content);
    /* A frame: the buttons pressed since the last one in, the screen drawn into
     * PS5_REMOTE_FRAME_WIDTH x PS5_REMOTE_FRAME_HEIGHT XRGB8888 pixels out. */
    void ps5_remote_core_frame(uint32_t pressed, uint32_t *pixels);
    /* 1 once the screen is done: the core closes its content. In game mode it stays 0: the
     * title has RetroArch quit then, for the frontend to come back. */
    int ps5_remote_core_finished(void);
    /* The content is closed. */
    void ps5_remote_core_close(void);

#ifdef __cplusplus
}
#endif

#endif
