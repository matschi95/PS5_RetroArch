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
 * Downloads are written by the console's FTP server (ftpsrv, etaHEN's...), through the
 * platform's offload streams (ps5platform/offload.h): they need one.
 *
 * The save sync (src/remote/save_jobs.h): RetroArch's hook as a game's content loads
 * (ps5_save_sync_before, patches/series 0117) syncs its save data first, and once its core
 * closed again it goes up (ps5_remote_core_closed); what waits from an earlier start goes
 * on as RetroArch starts (ps5_remote_start). Notices are RetroArch's on-screen messages.
 *
 * Firmware (src/remote/firmware.h): as a game's core loads, what its info names and its
 * system folder lacks comes from the sources that listed it (ps5_remote_core_opening).
 */
#include "remote_core.h"

#include "../frontend_mode_ps5.h"
#include "../ps5_game.h"
#include "../ps5_library.h"
#include "configuration.h"
#include "files.h"
#include "library.h"
#include "remote.h"
#include "save_config.h"
#include "pairing.h"
#include "save_jobs.h"
#include "ui/remote_ui.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <ps5platform/offload.h>
#include <queues/message_queue.h>
#include <unistd.h>

extern "C" const char *ps5_core_system_directory(const char *core, const char *directory);
extern "C" void runloop_msg_queue_push(const char *msg, size_t len, unsigned prio,
                                       unsigned duration, bool flush, char *title,
                                       enum message_queue_icon icon,
                                       enum message_queue_category category);

namespace
{
using namespace ps5::remote;

/* The most a game waits for its save data before it starts. */
constexpr unsigned sync_timeout = 15;

/* How long the start waits for a new source's answers. */
constexpr unsigned start_timeout = 5;

/* The most a core's loading waits for its firmware. */
constexpr auto firmware_timeout = std::chrono::seconds(20);

bool started = false;

bool is_remote_core(const char *path)
{
    return ps5_library_is_fetch_core(path) != 0;
}

/* A download's file written by the console's FTP server: the platform finds the server and
 * the folder it sees /app0 as (ps5_offload_setup, once), and a stream sends the bytes there.
 * /app0, not content/: the setup looks one folder deep in the homebrew folders, where the
 * title's own is (/data/homebrew/PPSA99169), and content/ is one deeper. Cut at the offset
 * first, the file is the title's own: the server appends to it. */
class OffloadWriter final : public Writer
{
  public:
    ~OffloadWriter() override
    {
        if (stream_)
            (void)ps5_offload_end(stream_);
    }
    bool open(const std::string &path, uint64_t offset, std::string *error) override
    {
        static std::once_flag once;
        static bool ready = false;
        std::call_once(once, [] { ready = ps5_offload_setup("/app0", nullptr, 0) == 0; });
        if (!ready)
        {
            *error = "Downloads need an FTP server running on the console (ftpsrv, etaHEN's...); "
                     "none answers. Start one, then reopen the title";
            return false;
        }
        if (stream_)
            (void)ps5_offload_end(stream_);
        stream_ = nullptr;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0666);
        const bool cut = fd >= 0 && ftruncate(fd, static_cast<off_t>(offset)) == 0;
        if (fd >= 0)
            close(fd);
        stream_ = cut ? ps5_offload_begin(path.c_str(), offset) : nullptr;
        if (!stream_)
            *error = "The console's FTP server could not write " + path;
        return stream_ != nullptr;
    }
    bool write(const void *data, size_t size, std::string *error) override
    {
        if (ps5_offload_write(stream_, data, size) == 0)
            return true;
        *error = "The console's FTP server could not write the file (is the drive full?)";
        return false;
    }
    bool finish(std::string *error) override
    {
        const bool whole = stream_ && ps5_offload_end(stream_) == 0;
        stream_ = nullptr;
        if (!whole)
            *error = "The console's FTP server could not write the file (is the drive full?)";
        return whole;
    }

  private:
    struct ps5_offload *stream_ = nullptr;
};

/* An on-screen message; warning: something the player has to see to. */
void notice(const std::string &text, bool warning)
{
    runloop_msg_queue_push(text.c_str(), text.size(), 1, 300, false, nullptr,
                           MESSAGE_QUEUE_ICON_DEFAULT,
                           warning ? MESSAGE_QUEUE_CATEGORY_WARNING : MESSAGE_QUEUE_CATEGORY_INFO);
}

/* A core's system folder as RetroArch gives it to the core (patches/series 0106: the Saturn
 * core's own); "" when RetroArch's setting is empty (the content's folder then). */
std::string firmware_folder(const std::string &core)
{
    const settings_t *settings = config_get_ptr();
    const char *system = settings ? settings->paths.directory_system : "";
    if (!system || !*system)
        return "";
    return ps5_core_system_directory(core.c_str(), system);
}

/* The title's paths, its downloads written by the console's FTP server. */
Paths title_paths()
{
    Paths paths = Paths::title();
    paths.writer = [] { return std::unique_ptr<Writer>(new OffloadWriter); };
    paths.firmware_folder = firmware_folder;
    return paths;
}

/* What a core's info names and its system folder lacks, from the sources' firmware. */
void fetch_core_firmware(const char *core_path)
{
    std::string name = files::base_name(core_path);
    if (const size_t dot = name.rfind(".so"); dot != std::string::npos)
        name.resize(dot);
    CoreFirmware core;
    if (!read_core_firmware(Paths::title().info + "/" + name + ".info", &core))
        return;
    const std::string folder = firmware_folder(core.core);
    if (folder.empty() || core.files.empty())
        return;
    const auto ends = std::chrono::steady_clock::now() + firmware_timeout;
    const std::vector<FirmwareOffer> offers = firmware_offers();
    const FirmwareOutcome outcome = fetch_firmware(
        core, folder, offers, [ends] { return std::chrono::steady_clock::now() > ends; });
    if (const std::string text = firmware_notice(outcome, !offers.empty()); !text.empty())
    {
        std::fprintf(stderr, "[firmware] %s: %s\n", core.core.c_str(), text.c_str());
        notice(text, !outcome.failed.empty() || !outcome.missing.empty());
    }
}

SaveJobs &save_jobs();

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
        forget(game.source, game.id);
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
        /* Its save data synced here, asking: RetroArch's hook leaves it alone. */
        if (syncing_.content == launch)
            save_jobs().synced_launch(launch);
        /* The game goes into its system's playlist at this restart (src/remote/library.h),
         * which associates it with the platform's core. */
        ps5_game_launch(&request);
        return request.error;
    }
    bool sync_wanted(const std::string &launch) override
    {
        SyncGame game;
        return save_jobs().wanted() && save_jobs().known(launch, &game);
    }
    void sync_start(const std::string &launch) override
    {
        if (!save_jobs().known(launch, &syncing_))
            syncing_ = SyncGame();
        waiting_ = true;
        sync_view();
    }
    SyncView sync_view() override
    {
        /* Another sync under way (a game's after it ended): this one once it is done. */
        if (waiting_ && save_jobs().start({syncing_}, true))
            waiting_ = false;
        if (!waiting_)
            return save_jobs().view();
        SyncView view;
        view.stage = SyncView::working;
        view.before = true;
        view.game = syncing_.name;
        return view;
    }
    void sync_choose(SyncChoice choice) override
    {
        save_jobs().choose(choice);
    }
    void sync_stop() override
    {
        waiting_ = false;
        syncing_ = SyncGame();
        save_jobs().stop();
    }
    void sync_end() override
    {
        save_jobs().end();
    }
    ui::SaveSetup save_setup() override
    {
        const std::string config = Paths::title().config;
        const SaveConfig read = read_save_config(config + "/save-sync.json");
        ui::SaveSetup setup;
        setup.readable = read.readable;
        setup.error = read.error;
        setup.type = read.type;
        setup.url = read.settings["url"].str();
        setup.user = read.settings["__server_user"].str();
        setup.automatic = read.automatic;
        setup.states = read.states;
        setup.waiting = save_jobs().pending().size();
        setup.servers = pair_servers(config + "/sources.json");
        return setup;
    }
    void pair_start(const PairServer &server) override
    {
        pairing_run().start(server.type, server.settings);
    }
    PairingRun::View pairing() override
    {
        return pairing_run().view();
    }
    void pair_cancel() override
    {
        pairing_run().cancel();
    }
    bool unlink(std::string *error) override
    {
        if (set_save_server(Paths::title().config + "/save-sync.json", Json::record()))
            return true;
        *error = "save-sync.json could not be written";
        return false;
    }
    double now() override
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

  private:
    PairingRun &pairing_run()
    {
        if (!pairing_)
        {
            const std::string config = Paths::title().config;
            pairing_.reset(new PairingRun([this] { return now(); }, config + "/save-sync.json",
                                          config + "/save-sync"));
        }
        return *pairing_;
    }

    std::unique_ptr<PairingRun> pairing_;
    SyncGame syncing_;     /* the game whose save data syncs before it starts from here */
    bool waiting_ = false; /* its sync is still to start */
};

/* The save sync's jobs: they live as long as the process (their thread uses them). */
SaveJobs &save_jobs()
{
    static SaveJobs *jobs = []
    {
        SaveJobs *made = new SaveJobs(Paths::title().config);
        made->on_notice(
            [](const std::string &text)
            {
                notice(text, text.find("not synced") != std::string::npos ||
                                 text.find("too old") != std::string::npos);
            });
        return made;
    }();
    return *jobs;
}

/* A game as the save sync takes it: its name, platform and checksum from its playlist. */
SyncGame sync_game_of(const char *content, const char *core, const char *savefile,
                      const char *savestate)
{
    SyncGame game;
    game.content = content;
    game.emulator = emulator_of(core);
    game.save = savefile ? savefile : "";
    game.state = savestate ? savestate : "";
    game.name = files::base_name(game.content);
    if (const size_t dot = game.name.rfind('.'); dot != std::string::npos && dot > 0)
        game.name.resize(dot);
    const Paths paths = Paths::title();
    struct ps5_library library;
    if (ps5_library_load(&library, paths.playlists.c_str(), paths.info.c_str(),
                         paths.cores.c_str()) == 0)
        for (size_t i = 0; i < library.game_count; i++)
            if (game.content == library.games[i].path)
            {
                const struct ps5_library_system &system = library.systems[library.games[i].system];
                game.name = library.games[i].label;
                game.crc32 = library.games[i].crc32;
                if (system.known)
                    game.platform = system.id;
                break;
            }
    ps5_library_free(&library);
    return game;
}

/* The game whose core is open, for its save data after it ended. */
std::mutex playing_lock;
SyncGame playing;
std::string playing_core;

TitleServices services;
std::unique_ptr<ui::Screen> screen;

void start_threads()
{
    const Paths paths = title_paths();
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
    /* save-sync.json for the player to fill in, and what waits since an earlier start. */
    if (!prepare_save_config(Paths::title().config + "/save-sync.json"))
        std::fprintf(stderr, "[save sync] save-sync.json is not readable as JSON: it is left as "
                             "it is\n");
    save_jobs().resume_pending();
}

extern "C" void ps5_save_sync_before(const char *content, const char *core, const char *savefile,
                                     const char *savestate)
{
    if (!content || !*content || !core || is_remote_core(core) || !savefile || !*savefile)
        return;
    SaveJobs &jobs = save_jobs();
    if (!jobs.wanted())
        return;
    const SyncGame game = sync_game_of(content, core, savefile, savestate);
    {
        std::lock_guard<std::mutex> guard(playing_lock);
        playing = game;
        playing_core = core;
    }
    jobs.sync_before(game, sync_timeout);
}

extern "C" void ps5_remote_core_opening(const char *path)
{
    if (!started || is_remote_core(path))
        return;
    stop();
    fetch_core_firmware(path);
}

extern "C" void ps5_remote_core_closed(const char *path)
{
    /* Its save files are written: they go up. */
    SyncGame ended;
    {
        std::lock_guard<std::mutex> guard(playing_lock);
        if (path && playing_core == path)
        {
            ended = playing;
            playing = SyncGame();
            playing_core.clear();
        }
    }
    if (!ended.content.empty())
        save_jobs().played(ended);
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
