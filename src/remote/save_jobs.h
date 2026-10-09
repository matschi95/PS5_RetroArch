/* PS5 RetroArch - when the save sync runs: before a game starts and after it ended.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The same for every way a game starts. Started from RetroArch's menu, the title's hook syncs
 * the game's save data as it loads, without asking (sync_before): what cannot be done then
 * (the server does not answer, both sides changed) is left as it is and said, for the remote
 * core's Save sync. Started from the remote core, its screen syncs first, asking (start with
 * before set), and the hook leaves that game alone (synced_launch). After a game, once its
 * core closed, its save data goes up on a thread of its own (played), and is tried again at
 * later starts until it worked (<folder>/pending.json). save-sync.json's "auto": false stops
 * all of them.
 */
#pragma once

#include "save_sync.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ps5::remote
{
/* Where a sync is, for a screen to show. */
struct SyncView
{
    enum Stage : uint8_t
    {
        idle,
        working,  /* syncing (game: which) */
        conflict, /* both changed: the player is asked (the times, the server's device) */
        done,     /* outcome says how it ended */
        failed,   /* error says why */
    } stage = idle;
    bool before = false; /* before a game starts (else after one ended) */
    std::string game;
    std::string server; /* "alex @ http://nas:3000" */
    std::string file;   /* conflict: the file asked about */
    int64_t console_time = 0, server_time = 0;
    std::string server_device;
    SyncOutcome outcome = SyncOutcome::same;
    std::string error;
    bool too_old = false;
    std::string server_version, needed_version;
};

class SaveJobs
{
  public:
    /* folder: config/remote (save-sync.json, and save-sync/ for what it keeps). */
    explicit SaveJobs(std::string folder);
    ~SaveJobs();
    SaveJobs(const SaveJobs &) = delete;
    SaveJobs &operator=(const SaveJobs &) = delete;

    /* Whether a game's save data syncs at all: a server set, "auto" not off. */
    bool wanted() const;
    /* Told of what a sync did (a notice on screen), from the thread that ran it. */
    void on_notice(std::function<void(const std::string &)> notice);

    /* Before a game, without asking (RetroArch's menu): blocking, `seconds` at most. A game the
     * remote core synced just now (synced_launch) is left alone. */
    SyncResult sync_before(const SyncGame &game, unsigned seconds);
    /* The remote core synced `content` and starts it: the hook leaves it alone, once. */
    void synced_launch(const std::string &content);
    /* A game as the hook last had it (its save file's and states' names are RetroArch's, known
     * once it loaded): false when it was never played on the console, which then has none of
     * its save data, so the hook's sync needs to ask nothing. */
    bool known(const std::string &content, SyncGame *game) const;

    /* A sync on the thread: games before one starts (the player is asked in a conflict: view,
     * choose), or those waiting since they were played (a conflict is left as it is). False when
     * another one is under way (it is not waited for: a screen asks again). */
    bool start(std::vector<SyncGame> games, bool before);
    /* After a game: its save data goes up (and waits in pending.json until it did). */
    void played(const SyncGame &game);
    /* What waits since an earlier start. */
    void resume_pending();
    SyncView view();
    void choose(SyncChoice choice);
    /* Stops the sync on the thread (the player went back, a game starts): what it sends
     * stops; a conflict not answered is left as it is. */
    void stop();
    /* A finished sync's view is cleared. */
    void end();
    /* Waits for the thread to end. */
    void wait();

    /* ---- the parts, for tests ---- */
    std::vector<SyncGame> pending() const;
    static Json to_json(const SyncGame &game);
    static SyncGame from_json(const Json &json);

  private:
    SyncPlaces places(bool states) const;
    void write_pending(const std::vector<SyncGame> &games) const;
    void remember(const SyncGame &game) const;
    void notice(const std::string &text);

    const std::string folder_;
    std::mutex lock_;
    std::condition_variable changed_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    SyncView view_;
    std::optional<SyncChoice> choice_;
    std::function<void(const std::string &)> notice_;
    mutable std::mutex pending_lock_;
};

/* What a sync did, as a notice says it ("Chrono Trigger: save data uploaded"). */
std::string sync_notice(const std::string &game, const SyncResult &result, bool before);
} // namespace ps5::remote
