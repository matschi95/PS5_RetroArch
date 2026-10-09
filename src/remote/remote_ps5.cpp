/* PS5 RetroArch - the download sources in the title: when they run, the remote core's
 * screens (src/remote/remote_core.h) and what the title does for them.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * As eboot.bin starts, a source set up since the last start is listed behind the shell's
 * splash (ps5_remote_list_new), and the playlists are written before the frontend dispatch
 * (ps5_remote_sync). While RetroArch runs, the lists are read and the queue downloads
 * (ps5_remote_start); a game's core loading stops that until it is closed again
 * (ps5_remote_core_opening, from src/core_loader_ps5.cpp), the remote core's does not.
 */
#include "remote_core.h"

#include "../frontend_mode_ps5.h"
#include "../ps5_game.h"
#include "../ps5_library.h"
#include "files.h"
#include "library.h"
#include "remote.h"
#include "ui/remote_ui.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
using namespace ps5::remote;

/* How long the start waits for a new source's answers. */
constexpr unsigned start_timeout = 5;

bool started = false;

bool is_remote_core(const char *path)
{
    return ps5_library_is_fetch_core(path) != 0;
}

/* The title's services for the remote core's screens. */
class TitleServices final : public ui::Services
{
  public:
    Status status() override
    {
        return current();
    }
    std::vector<Title> titles() override
    {
        return ps5::remote::titles();
    }
    std::vector<Download> downloads() override
    {
        return ps5::remote::downloads();
    }
    bool enqueue(const std::string &source, const std::string &id, bool first) override
    {
        return ps5::remote::enqueue(source, id, first);
    }
    bool cancel(const std::string &source, const std::string &id) override
    {
        return ps5::remote::cancel(source, id);
    }
    void refresh() override
    {
        ps5::remote::refresh();
    }
    std::string placed(const std::string &source, const std::string &id) override
    {
        return placed_folder(source, id);
    }
    bool remove(const Game &game, const std::string &folder, std::string *error) override
    {
        files::remove_tree(folder);
        if (files::is_folder(folder))
        {
            *error = "the folder " + folder + " could not be deleted whole";
            return false;
        }
        note_removed(Paths::title(), game.platform, folder + "/" + game.file);
        changed();
        return true;
    }
    std::string play(const Game &, const std::string &launch, const std::string &core) override
    {
        /* Back to the frontend the game was chosen in: the one that asked game mode for the
         * stub, or RetroArch's menu. */
        struct ps5_game request = {};
        std::snprintf(request.content, sizeof request.content, "%s", launch.c_str());
        std::snprintf(request.core, sizeof request.core, "%s", core.c_str());
        if (const struct ps5_game *running = ps5_frontend_game())
        {
            std::snprintf(request.frontend, sizeof request.frontend, "%s", running->frontend);
            std::snprintf(request.state, sizeof request.state, "%s", running->state);
        }
        else
            std::snprintf(request.frontend, sizeof request.frontend, "%s", PS5_GAME_EBOOT);
        /* The game goes into its system's playlist at this restart (src/remote/library.h),
         * which associates it with the platform's core. */
        ps5_game_launch(&request);
        return request.error;
    }
    double now() override
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

TitleServices services;
std::unique_ptr<ui::Screen> screen;

void start_threads()
{
    const Paths paths = Paths::title();
    start(paths, [paths](const Placed &placed) { note_placed(paths, placed); });
}
} // namespace

extern "C" void ps5_remote_list_new(void)
{
    list_new(Paths::title(), start_timeout);
}

extern "C" void ps5_remote_sync(void)
{
    sync(Paths::title());
}

extern "C" void ps5_remote_start(void)
{
    started = true;
    start_threads();
}

extern "C" void ps5_remote_core_opening(const char *path)
{
    if (started && !is_remote_core(path))
        stop();
}

extern "C" void ps5_remote_core_closed(const char *path)
{
    if (started && !is_remote_core(path))
        start_threads();
}

extern "C" int ps5_remote_core_open(const char *content)
{
    Stub stub;
    if (!content || !read_stub(content, &stub))
        return -1;
    screen = ui::open(stub, services);
    return 0;
}

extern "C" void ps5_remote_core_frame(uint32_t pressed, uint32_t *pixels)
{
    if (!screen)
        return;
    screen->input(pressed);
    ui::Canvas canvas(pixels);
    screen->draw(canvas);
}

extern "C" int ps5_remote_core_finished(void)
{
    return !screen || screen->finished();
}

extern "C" void ps5_remote_core_close(void)
{
    screen.reset();
}
