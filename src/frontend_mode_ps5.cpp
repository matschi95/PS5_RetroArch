/* PS5 RetroArch - which frontend a launch of eboot.bin starts, and game mode.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title has a pre-screen that chooses between RetroArch and EmulationStation
 * (docs/FRONTENDS.md), and each runs in a process of its own: the title restarts
 * itself through LoadExec (approach B). eboot.bin is what the home screen starts,
 * so it decides first, before RetroArch sets anything up:
 *
 *   --ps5-mode=retroarch   RetroArch, here (the picker chose it)
 *   --ps5-mode=game        a game a frontend asked for, in RetroArch, here
 *   --ps5-mode=es-de       EmulationStation, /app0/es-de/es-de.bin
 *   --ps5-mode=picker      the picker, /app0/picker/picker.bin
 *   --ps5-mode=quit        a frontend quit (EmulationStation, or RetroArch the
 *                          picker started): the picker, or, when a frontend is
 *                          remembered, the title closes
 *   no mode                a launch from the home screen: the frontend remembered
 *                          in config/frontend.cfg (src/ps5_frontend_choice.h), or
 *                          the picker when none is or L1 is held as the title
 *                          starts; but a test run's launch (/app0/test-run.txt,
 *                          which tools/run-title.sh writes) stays RetroArch, as
 *                          every test expects, unless a picker test is armed
 *                          (/app0/picker/picker-test.txt)
 *
 * Game mode is the one way any frontend starts a game (src/ps5_game.h has the
 * contract). Its request is taken here and checked. The core is the one RetroArch's
 * playlists associate with the content, when they do, else the frontend's. A request
 * that cannot
 * run is refused back to its frontend, with the reason in the result. RetroArch then
 * starts on the game (src/main.cpp adds -L, the content and Close Content quitting),
 * and once it has quit, ps5_frontend_after_retroarch writes the result and restarts
 * the title as the frontend.
 *
 * Quitting a frontend goes back to the picker: EmulationStation restarts eboot.bin
 * with no mode (frontends/es-de/ps5/main_ps5.cpp), and RetroArch, when the picker
 * started it, does the same once it has quit and closed its drivers. The picker's
 * CIRCLE closes the title. RetroArch started any other way (a test run) closes the
 * title as before.
 *
 * A title built without the picker, or without EmulationStation, runs RetroArch
 * as it always has. LoadExec, accepted, returns and the shell replaces the process a
 * moment later (evidence/loadexec-relaunch): this waits for that, up to a minute. A
 * LoadExec refused, or a process still here after the wait, is recorded in the trace
 * and RetroArch runs instead, so a launch never ends on a black screen.
 */
#include "frontend_mode_ps5.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

#include "ps5_frontend_choice.h"
#include "trace.hpp"

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);

namespace ps5::frontend_mode
{
namespace
{
/* This launch's --ps5-mode, kept for the quit */
std::string launch_mode;
/* The game this launch runs, in game mode */
struct ps5_game game;
bool game_running = false;
std::time_t game_started = 0;

extern "C" void ps5_permissions_settle(); /* src/permissions_ps5.cpp */

bool exists(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file)
        std::fclose(file);
    return file != nullptr;
}

/* Restarts the title as the image, with one argument; it returns only when that did
 * not happen. */
void restart_as(const std::string &image, const char *argument, unsigned replaced_wait_seconds,
                const char *failure)
{
    const char *const arguments[] = {argument, nullptr};
    ps5_permissions_settle();
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec(image.c_str(), arguments);
    if (result >= 0)
        for (unsigned waited = 0; waited < replaced_wait_seconds * 10; waited++)
            usleep(100000);
    ps5::debug::mark_value(failure, result);
}

void restart_as(const std::string &image, unsigned replaced_wait_seconds, const char *failure)
{
    restart_as(image, "", replaced_wait_seconds, failure);
}

/* Asks the shell to close the title, as RetroArch's quit does; it returns only when
 * that did not happen. */
void close_title(unsigned replaced_wait_seconds)
{
    ps5_permissions_settle();
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec("exit", nullptr);
    if (result >= 0)
        for (unsigned waited = 0; waited < replaced_wait_seconds * 10; waited++)
            usleep(100000);
    ps5::debug::mark_value("frontend: the shell did not close the title; RetroArch runs; result",
                           result);
}

/* A request refused: its frontend gets the reason, and the title goes back to it. */
void refuse(const Paths &paths, struct ps5_game &request, unsigned replaced_wait_seconds)
{
    char line[400];
    std::snprintf(line, sizeof line, "game mode: refused: %s", request.error);
    ps5::debug::mark(line);
    request.status = -1;
    request.seconds = 0;
    if (request.frontend[0] != '/' || !exists(request.frontend) ||
        ps5_game_write(paths.result.c_str(), &request, 1) != 0)
        return; /* nowhere to go back to: RetroArch runs */
    restart_as(
        request.frontend, replaced_wait_seconds,
        "game mode: LoadExec of the frontend did not replace the process; RetroArch runs; result");
}

/* Takes the request: true when RetroArch is to run its game in this process. */
bool take_game(const Paths &paths, unsigned replaced_wait_seconds)
{
    int kind = -1;
    struct ps5_game request;
    if (ps5_game_read(paths.request.c_str(), &request, &kind) != 0 || kind != 0)
    {
        ps5::debug::mark("game mode: no request; RetroArch runs");
        return false;
    }
    std::remove(paths.request.c_str()); /* a request runs once */
    if (ps5_game_check(&request) != 0)
    {
        refuse(paths, request, replaced_wait_seconds);
        return false;
    }
    /* RetroArch's playlists decide first: a core they associate with this content is
     * the one RetroArch would use, so it wins over the frontend's default for the
     * system, if it is one of the title's cores. */
    struct ps5_game associated = request;
    if (ps5_game_playlist_core(paths.playlists.c_str(), request.content, associated.core,
                               sizeof(associated.core)) &&
        std::strcmp(associated.core, request.core) != 0)
    {
        if (ps5_game_check(&associated) == 0)
        {
            ps5::debug::mark(
                "game mode: the core RetroArch's playlist associates with the content");
            std::snprintf(request.core, sizeof(request.core), "%s", associated.core);
        }
        else
            ps5::debug::mark(
                "game mode: the playlist's core is not one of the title's; kept the frontend's");
    }
    if (!request.core[0])
    {
        std::snprintf(
            request.error, sizeof(request.error),
            "no core: neither the frontend nor RetroArch's playlists name one for this content");
        refuse(paths, request, replaced_wait_seconds);
        return false;
    }
    game = request;
    game_running = true;
    game_started = std::time(nullptr);
    char line[PS5_GAME_PATH_MAX * 3 + 64];
    std::snprintf(line, sizeof line, "game mode: %s with %s, for %s", game.content, game.core,
                  game.frontend);
    ps5::debug::mark(line);
    return true;
}
} // namespace

std::string mode_argument(int argc, char **argv)
{
    static const char prefix[] = "--ps5-mode=";
    for (int i = 0; i < argc && argv && argv[i]; i++)
        if (std::strncmp(argv[i], prefix, sizeof(prefix) - 1) == 0)
            return argv[i] + sizeof(prefix) - 1;
    return "";
}

bool session_start(int argc, char **argv)
{
    for (int i = 0; i < argc && argv && argv[i]; i++)
        if (std::strncmp(argv[i], "--ps5-", 6) == 0)
            return false;
    return true;
}

void rotate(const std::string &path, const std::string &previous)
{
    if (!exists(path))
        return;
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
}

namespace
{
/* "/app0/retroarch.log" -> "/app0/retroarch.1.log" */
std::string previous_of(const std::string &path)
{
    const std::string::size_type dot = path.rfind('.');
    const std::string::size_type slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + ".1";
    return path.substr(0, dot) + ".1" + path.substr(dot);
}
} // namespace

void start_session_logs(const Paths &paths, int argc, char **argv)
{
    if (session_start(argc, argv))
        rotate(paths.trace, previous_of(paths.trace));
}

std::string retroarch_log(const Paths &paths)
{
    const std::string &log = game_running ? paths.game_log : paths.retroarch_log;
    rotate(log, previous_of(log));
    return log;
}

Next decide(const Launch &launch)
{
    if (launch.mode == "game")
        return Next::game;
    if (launch.mode == "es-de")
        return launch.es_de_present ? Next::es_de : Next::retroarch;
    if (launch.mode == "picker")
        return launch.picker_present ? Next::picker : Next::retroarch;
    if (launch.mode == "quit")
        return launch.choice == "ask" && launch.picker_present ? Next::picker : Next::close;
    if (!launch.mode.empty())
        return Next::retroarch; /* retroarch, or a mode this build does not know */
    if (launch.picker_test && launch.picker_present)
        return Next::picker;
    if (launch.test_run)
        return Next::retroarch;
    if (!launch.reopen_held)
    {
        if (launch.choice == "retroarch")
            return Next::retroarch;
        if (launch.choice == "es-de" && launch.es_de_present)
            return Next::es_de;
    }
    return launch.picker_present ? Next::picker : Next::retroarch;
}

const char *name(Next next)
{
    switch (next)
    {
    case Next::picker:
        return "picker";
    case Next::es_de:
        return "es-de";
    case Next::game:
        return "game";
    case Next::close:
        return "close";
    default:
        return "retroarch";
    }
}

void run(const Paths &paths, int argc, char **argv, unsigned replaced_wait_seconds)
{
    Launch launch;
    launch.mode = mode_argument(argc, argv);
    launch_mode = launch.mode;
    launch.test_run = exists(paths.test_run);
    launch.picker_test = exists(paths.picker_test);
    launch.picker_present = exists(paths.picker);
    launch.es_de_present = exists(paths.es_de);
    launch.choice = ps5_frontend_choice_read(paths.choice.c_str());
    /* The pad is read only when what it decides is in question: a launch from the
     * home screen, with a frontend remembered. */
    if (launch.mode.empty() && !launch.test_run && !launch.picker_test && launch.choice != "ask" &&
        launch.picker_present && paths.reopen_held)
        launch.reopen_held = paths.reopen_held();
    const Next next = decide(launch);
    char line[256];
    std::snprintf(line, sizeof line,
                  "frontend: mode '%s', test run %d, picker test %d, picker %d, es-de %d, "
                  "remembered %s, L1 %d -> %s",
                  launch.mode.c_str(), launch.test_run, launch.picker_test, launch.picker_present,
                  launch.es_de_present, launch.choice.c_str(), launch.reopen_held, name(next));
    ps5::debug::mark(line);
    if (next == Next::game)
    {
        take_game(paths, replaced_wait_seconds);
        return; /* RetroArch runs: the game, or as it always has */
    }
    if (next == Next::retroarch)
        return;
    if (next == Next::close)
    {
        close_title(replaced_wait_seconds);
        return;
    }
    restart_as(next == Next::picker ? paths.picker : paths.es_de, replaced_wait_seconds,
               "frontend: LoadExec did not replace the process; RetroArch runs; result");
}

const struct ps5_game *running_game()
{
    return game_running ? &game : nullptr;
}

void replace_game(const char *content)
{
    if (game_running && content && *content)
        std::snprintf(game.content, sizeof game.content, "%s", content);
}

bool back_to_picker(const std::string &mode, bool picker_present)
{
    return mode == "retroarch" && picker_present;
}

void after_retroarch(const Paths &paths, const std::string &mode, int status,
                     unsigned replaced_wait_seconds, bool update_installed)
{
    if (update_installed)
    {
        /* The new build's files are in place: starting a frontend now would run the
         * new picker or EmulationStation beside this old process's decisions. The
         * title closes, and the updater's message asks for it to be reopened. */
        ps5::debug::mark("frontend: an update was installed; the title closes, no handover");
        game_running = false;
        return;
    }
    if (game_running)
    {
        game_running = false;
        game.status = status;
        game.seconds = static_cast<long>(std::time(nullptr) - game_started);
        game.error[0] = '\0';
        char line[128];
        std::snprintf(line, sizeof line,
                      "game mode: RetroArch quit (status %d) after %ld s; back to the frontend",
                      status, game.seconds);
        ps5::debug::mark(line);
        if (ps5_game_write(paths.result.c_str(), &game, 1) != 0)
            ps5::debug::mark("game mode: the result could not be written");
        restart_as(game.frontend, replaced_wait_seconds,
                   "game mode: LoadExec of the frontend did not replace the process; the title "
                   "closes; result");
        return;
    }
    if (!back_to_picker(mode, exists(paths.picker)))
        return;
    /* A frontend quit: the picker, unless one is remembered by now (its Remember
     * switch), in which case the title closes, as RetroArch alone always did. */
    ps5::debug::mark("frontend: RetroArch quit; back to the picker or closed");
    restart_as(paths.eboot, "--ps5-mode=quit", replaced_wait_seconds,
               "frontend: LoadExec did not replace the process; the title closes; result");
}
} // namespace ps5::frontend_mode

namespace
{
const ps5::frontend_mode::Paths title_paths{"/app0/picker/picker.bin",
                                            "/app0/es-de/es-de.bin",
                                            "/app0/test-run.txt",
                                            "/app0/picker/picker-test.txt",
                                            PS5_GAME_EBOOT,
                                            PS5_GAME_REQUEST_PATH,
                                            PS5_GAME_RESULT_PATH,
                                            PS5_GAME_PLAYLISTS,
                                            PS5_FRONTEND_CHOICE_PATH,
                                            "/app0/trace.txt",
                                            "/app0/retroarch.log",
                                            "/app0/retroarch-game.log",
                                            ps5_frontend_reopen_held};
std::string log_path;
} // namespace

extern "C" void ps5_frontend_session_logs(int argc, char **argv)
{
    ps5::frontend_mode::start_session_logs(title_paths, argc, argv);
}

extern "C" void ps5_frontend_dispatch(int argc, char **argv)
{
    ps5::frontend_mode::run(title_paths, argc, argv, 60);
}

extern "C" const char *ps5_frontend_retroarch_log(void)
{
    log_path = ps5::frontend_mode::retroarch_log(title_paths);
    return log_path.c_str();
}

extern "C" const struct ps5_game *ps5_frontend_game(void)
{
    return ps5::frontend_mode::running_game();
}

extern "C" void ps5_frontend_game_replaced(const char *content)
{
    ps5::frontend_mode::replace_game(content);
}

extern "C" void ps5_frontend_after_retroarch(int status, int update_installed)
{
    ps5::frontend_mode::after_retroarch(title_paths, ps5::frontend_mode::launch_mode, status, 60,
                                        update_installed != 0);
}
