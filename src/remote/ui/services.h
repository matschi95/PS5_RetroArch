/* PS5 RetroArch - what the remote core's screens (src/remote/ui/remote_ui.h) use: the
 * download sources, and what the title does for them (start a game, delete one).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title's own (src/remote/remote_ps5.cpp) goes to src/remote/remote.h; the
 * tests and the preview have sample ones (tests/remote_ui_test.cpp), so every screen can
 * be drawn without a server.
 */
#pragma once

#include "../remote.h"
#include "../pairing.h"
#include "../save_jobs.h"

#include <string>
#include <vector>

namespace ps5::remote::ui
{
/* The save sync's settings as the Save sync screen shows them (save-sync.json). */
struct SaveSetup
{
    bool readable = true; /* false: save-sync.json is no JSON (error) */
    std::string error;
    std::string type, url, user; /* the server; type "" when none is set */
    bool automatic = true, states = true;
    size_t waiting = 0;              /* games whose save data waits to go up */
    std::vector<PairServer> servers; /* the servers it can be paired with */
};

class Services
{
  public:
    virtual ~Services() = default;
    virtual Status status() = 0;
    virtual std::vector<Title> titles() = 0;
    virtual std::vector<Download> downloads() = 0;
    virtual bool enqueue(const std::string &source, const std::string &id, bool first) = 0;
    virtual bool cancel(const std::string &source, const std::string &id) = 0;
    virtual void refresh() = 0;
    /* A source's game's folder on the console; "" when it is not there. */
    virtual std::string placed(const std::string &source, const std::string &id) = 0;
    /* Deletes a downloaded game from the console: its folder (its saves stay). */
    virtual bool remove(const Game &game, const std::string &folder, std::string *error) = 0;
    /* Has game mode run a downloaded game with its platform's core; returns only when that did
     * not happen, with why. */
    virtual std::string play(const Game &game, const std::string &launch,
                             const std::string &core) = 0;
    /* The save sync before a game starts from here (src/remote/save_jobs.h): whether its save
     * data syncs first (a server is set, and the game was played on the console before),
     * the sync on its thread, where it is, the player's choice in a conflict, stopping it,
     * and done with it. */
    virtual bool sync_wanted(const std::string &launch) = 0;
    virtual void sync_start(const std::string &launch) = 0;
    virtual SyncView sync_view() = 0;
    virtual void sync_choose(SyncChoice choice) = 0;
    virtual void sync_stop() = 0;
    virtual void sync_end() = 0;
    /* The Save sync screen: the settings, a pairing with one of setup's servers on its thread
     * (pairing.h), where it is, cancelling it, and unlinking the server. */
    virtual SaveSetup save_setup() = 0;
    virtual void pair_start(const PairServer &server) = 0;
    virtual PairingRun::View pairing() = 0;
    virtual void pair_cancel() = 0;
    virtual bool unlink(std::string *error) = 0;
    /* Seconds, for how long a message stands. */
    virtual double now() = 0;
};
} // namespace ps5::remote::ui
