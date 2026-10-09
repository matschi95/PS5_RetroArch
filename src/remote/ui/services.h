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

#include <string>
#include <vector>

namespace ps5::remote::ui
{
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
    /* Seconds, for how long a message stands. */
    virtual double now() = 0;
};
} // namespace ps5::remote::ui
