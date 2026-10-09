/* PS5 RetroArch - the remote core: the download sources' games and screens.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The core of a download source's game in RetroArch's playlists (src/remote/library.h),
 * and of the download sources' own screens (Downloads). Its content is a stub. What it
 * shows is eboot.bin's (src/remote/remote_core.h, src/remote/ui/remote_ui.h): this core
 * only reads the pad, hands the buttons pressed and its frame through, and closes its
 * content once the screen is done.
 */
#include <libretro.h>
#include <stdint.h>
#include <string.h>

#include "remote/remote_core.h"

static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_input_poll_t input_poll;
static retro_input_state_t input_state;
static uint32_t frame[PS5_REMOTE_FRAME_WIDTH * PS5_REMOTE_FRAME_HEIGHT];
static uint32_t held;
static int closing;

/* RetroArch's pad buttons and the screens' (PlayStation's names: B is Cross, A Circle). */
static const struct
{
    unsigned id;
    uint32_t bit;
} buttons[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, PS5_REMOTE_UP},       {RETRO_DEVICE_ID_JOYPAD_DOWN, PS5_REMOTE_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, PS5_REMOTE_LEFT},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, PS5_REMOTE_RIGHT},
    {RETRO_DEVICE_ID_JOYPAD_B, PS5_REMOTE_CROSS},     {RETRO_DEVICE_ID_JOYPAD_A, PS5_REMOTE_CIRCLE},
    {RETRO_DEVICE_ID_JOYPAD_Y, PS5_REMOTE_SQUARE},    {RETRO_DEVICE_ID_JOYPAD_X, PS5_REMOTE_TRIANGLE},
};

void retro_set_environment(retro_environment_t cb)
{
    bool no_game = false;
    environment = cb;
    environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state = cb; }

void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Remote";
    info->library_version = "1";
    info->valid_extensions = "remote";
    info->need_fullpath = true;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = info->geometry.max_width = PS5_REMOTE_FRAME_WIDTH;
    info->geometry.base_height = info->geometry.max_height = PS5_REMOTE_FRAME_HEIGHT;
    info->geometry.aspect_ratio = (float)PS5_REMOTE_FRAME_WIDTH / (float)PS5_REMOTE_FRAME_HEIGHT;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

bool retro_load_game(const struct retro_game_info *game)
{
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!game || !game->path || !environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
        return false;
    /* A button held as the content starts is no press. */
    held = ~0u;
    closing = 0;
    return ps5_remote_core_open(game->path) == 0;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{
    (void)type;
    (void)info;
    (void)count;
    return false;
}

void retro_unload_game(void)
{
    ps5_remote_core_close();
}

void retro_run(void)
{
    uint32_t now = 0;
    size_t i;
    input_poll();
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (input_state(0, RETRO_DEVICE_JOYPAD, 0, buttons[i].id))
            now |= buttons[i].bit;
    ps5_remote_core_frame(now & ~held, frame);
    held = now;
    video(frame, PS5_REMOTE_FRAME_WIDTH, PS5_REMOTE_FRAME_HEIGHT,
          PS5_REMOTE_FRAME_WIDTH * sizeof(uint32_t));
    if (!closing && ps5_remote_core_finished())
    {
        closing = 1;
        environment(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

void retro_reset(void) {}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}
bool retro_unserialize(const void *data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id)
{
    (void)id;
    return NULL;
}
size_t retro_get_memory_size(unsigned id)
{
    (void)id;
    return 0;
}
