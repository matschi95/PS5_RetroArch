/* The remote core (cores/remote/remote_libretro.c), on the host: what it tells RetroArch, the
 * pad's buttons handed through as presses (one held as the content starts is none), its
 * frame, and its content closed once the screen is done. eboot.bin's side
 * (src/remote/remote_core.h) is this file's.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../cores/remote/remote_libretro.c"

static char opened[256];
static uint32_t pressed_log[8];
static int frames, shutdowns, closes, finished, held_buttons;

int ps5_remote_core_open(const char *content)
{
    snprintf(opened, sizeof opened, "%s", content);
    return strstr(content, ".remote") ? 0 : -1;
}

void ps5_remote_core_frame(uint32_t pressed, uint32_t *pixels)
{
    pressed_log[frames % 8] = pressed;
    pixels[0] = 0x123456;
    frames++;
}

int ps5_remote_core_finished(void)
{
    return finished;
}

void ps5_remote_core_close(void)
{
    closes++;
}

static bool environment_cb(unsigned command, void *data)
{
    (void)data;
    if (command == RETRO_ENVIRONMENT_SHUTDOWN)
        shutdowns++;
    return true;
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    assert(width == PS5_REMOTE_FRAME_WIDTH && height == PS5_REMOTE_FRAME_HEIGHT &&
           pitch == width * 4 && ((const uint32_t *)data)[0] == 0x123456);
}

static void poll_cb(void)
{
}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    return port == 0 && device == RETRO_DEVICE_JOYPAD && ((held_buttons >> id) & 1);
}

int main(void)
{
    struct retro_system_info info;
    retro_get_system_info(&info);
    assert(strcmp(info.valid_extensions, "remote") == 0 && info.need_fullpath);
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_input_poll(poll_cb);
    retro_set_input_state(input_cb);

    struct retro_game_info stub = {"/app0/content/SNES/.remote/Game.remote", NULL, 0, NULL};
    struct retro_game_info other = {"/app0/content/SNES/Game.sfc", NULL, 0, NULL};
    assert(!retro_load_game(&other));
    held_buttons = 1 << RETRO_DEVICE_ID_JOYPAD_B; /* Cross held as the content starts */
    assert(retro_load_game(&stub) && strcmp(opened, stub.path) == 0);
    retro_run();
    assert(pressed_log[0] == 0); /* no press */
    held_buttons = 0;
    retro_run();
    held_buttons = (1 << RETRO_DEVICE_ID_JOYPAD_A) | (1 << RETRO_DEVICE_ID_JOYPAD_Y) |
                   (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    retro_run();
    assert(pressed_log[2] == (PS5_REMOTE_CIRCLE | PS5_REMOTE_SQUARE | PS5_REMOTE_DOWN));
    retro_run();
    assert(pressed_log[3] == 0); /* held: pressed once */
    assert(shutdowns == 0);
    finished = 1;
    retro_run();
    retro_run();
    assert(shutdowns == 1); /* once */
    retro_unload_game();
    assert(closes == 1);
    puts("remote core: system info, presses, frames, closing PASS");
    return 0;
}
