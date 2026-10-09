/* PS5 RetroArch - what the remote core's screens (src/remote/ui/remote_ui.h) use: the
 * download sources, and what the title does for them (start a game, delete one, the save
 * sync's setup).
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
    /* A source's game's file on the console that is run; "" when it is not there. */
    virtual std::string placed(const std::string &source, const std::string &id) = 0;
    /* Deletes a downloaded game from the console: its file, or its folder (its saves stay). */
    virtual bool remove(const Game &game, const std::string &launch, std::string *error) = 0;
    /* Starts a downloaded game with its platform's core, in this RetroArch: the remote core
     * goes. */
    virtual void play(const std::string &launch, const std::string &core) = 0;
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
