/* PS5 RetroArch - when the save sync runs: before a game starts and after it ended.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The same for every way a game starts (a downloaded one too: it is started from its
 * system's list as any other). As it loads, the title's hook syncs the game's save data,
 * without asking (sync_before): what cannot be done then (the server does not answer, both
 * sides changed) is left as it is and said. After a game, once its core closed, its save
 * data goes up on a thread of its own (played), and is tried again at later starts until it
 * worked (<folder>/pending.json). save-sync.json's "auto": false stops all of them.
 */
#pragma once

#include "save_sync.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps5::remote
{
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

    /* Before a game, without asking: blocking, `seconds` at most. */
    SyncResult sync_before(const SyncGame &game, unsigned seconds);

    /* After a game: its save data goes up on the thread (and waits in pending.json until it
     * did; a conflict is left as it is). */
    void played(const SyncGame &game);
    /* What waits since an earlier start. */
    void resume_pending();
    /* Waits for the thread to end. */
    void wait();

    /* ---- the parts, for tests ---- */
    std::vector<SyncGame> pending() const;
    static Json to_json(const SyncGame &game);
    static SyncGame from_json(const Json &json);

  private:
    /* The games waiting since they were played, synced on the thread unless it is busy (they
     * wait for the next start then). */
    void start(std::vector<SyncGame> games);
    SyncPlaces places(bool states) const;
    void write_pending(const std::vector<SyncGame> &games) const;
    void notice(const std::string &text);

    const std::string folder_;
    std::mutex lock_;
    std::thread thread_;
    std::atomic<bool> busy_{false}, stop_{false};
    std::function<void(const std::string &)> notice_;
    mutable std::mutex pending_lock_;
};

/* What a sync did, as a notice says it ("Chrono Trigger: save data uploaded"). */
std::string sync_notice(const std::string &game, const SyncResult &result, bool before);
} // namespace ps5::remote
