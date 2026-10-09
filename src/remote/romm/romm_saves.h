/* PS5 RetroArch - the RomM backend of the save stores (src/remote/save_store.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's save data on a RomM server, as the user the entry in save-sync.json signs in as,
 * with the server as romm_client.h has it:
 *
 *   "type": "romm",
 *   "url": "http://192.168.1.20:3000",
 *   "token": "rmm_...",                      a RomM client API token with the scopes
 *                                            platforms.read, roms.read, assets.read/write
 *                                            and devices.read/write,
 *   "username": "me", "password": "secret",  or the account's name and password instead
 *
 * As RomM keeps RetroArch's: a game's save is in the slot "autosave" of the emulator that is
 * the core ("snes9x"), its save states are RomM's states of that emulator by their file names
 * (".state", ".state1", ".state.auto"). It uses RomM's save sync (RomM 5.0 and newer): the
 * console is a device of the user (registered once, kept in the store's folder), and the save's
 * versions are the server's (it keeps the last ten of the slot). RomM keeps no hash of a state:
 * a state's version is its id and when it was last written.
 */
#pragma once

#include "../save_store.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ps5::remote::romm
{
/* The oldest RomM the save sync takes: the oldest it was checked with (tools/check-romm.py). An
 * older server is not synced with; every sync says so. */
inline constexpr char minimum_version[] = "5.0.0";

std::unique_ptr<SaveStore> make_saves(const Json &settings, const std::string &folder,
                                      std::string *error);

/* ---- the parts, for tests ---- */
/* A time as RomM writes it ("2026-10-08T12:34:56.123456+00:00", "...Z", or without a zone:
 * UTC), in seconds since 1970; 0 when it is none. */
int64_t parse_time(const std::string &text);
/* A time as RomM reads it ("2026-10-08T12:34:56Z"). */
std::string format_time(int64_t seconds);
/* Whether a version RomM's heartbeat says ("4.9.0", "5.0.0-beta.1") is minimum_version or
 * newer; a version that is no number ("development") is taken to be. */
bool can_sync(const std::string &version);
} // namespace ps5::remote::romm
