/* PS5 RetroArch - the save sync's settings: where the console keeps its save data besides
 * itself, in config/remote/save-sync.json.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title writes the file with an empty server; the player fills it in over FTP (like
 * sources.json) with a server and its sign-in, or pairs one in the remote core's Save sync:
 *
 *   { "version": 1,
 *     "auto": true,                      sync before a game starts and after it ends
 *     "states": true,                    the save states too, not only the saves
 *     "type": "romm",                    the backend (src/remote/backends.h); "": none
 *     "url": "http://192.168.1.20:3000",
 *     "token": "rmm_..." }               and what else the backend reads
 *
 * Fields that start with "__" are the title's, there to be read: what is written into them
 * changes nothing and is written over. The file says nothing about the download sources: the
 * save data may be kept on another server, or as another user of the same one, than the games
 * come from. What the player wrote stays as it is, fields the title does not know and their
 * order included; a file that is not JSON is left alone, and nothing syncs until it is
 * readable again.
 */
#pragma once

#include "source.h"

#include <string>

namespace ps5::remote
{
struct SaveConfig
{
    bool readable = false; /* false: the file is there but no JSON object (error says what) */
    std::string error;
    bool automatic = true; /* "auto" */
    bool states = true;    /* "states" */
    std::string type;      /* "" when no server is set */
    Json settings;         /* the whole file, which the backend reads its settings from */
};

/* The file as it is now (none: readable, no server). */
SaveConfig read_save_config(const std::string &file);
/* Makes the file when there is none, and adds what it lacks of the fields above. False when it
 * could not be read or written; it is then as it was. */
bool prepare_save_config(const std::string &file);
/* Sets what the file says about its server (pairing): `fields` ("type", "url", "token",
 * "__server_user"...) replace its sign-in, and a sign-in typed in before ("token",
 * "username", "password", "__server_user") goes; the player's own fields stay. Empty fields
 * unlink it (type, url and token empty). */
bool set_save_server(const std::string &file, const Json &fields);
} // namespace ps5::remote
