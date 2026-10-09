/* PS5 RetroArch - the RomM backend of the download sources (https://github.com/rommapp/romm).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The games of a RomM server, of all its platforms (or those an entry names). Its entry in
 * config/remote/sources.json, with the server as romm_client.h has it:
 *
 *   { "type": "romm", "name": "Home",
 *     "url": "http://192.168.1.20:3000",
 *     "token": "rmm_...",                      a RomM client API token (Profile > Client
 *                                              tokens),
 *     "username": "me", "password": "secret",  or the account's name and password instead
 *     "platforms": ["snes", "psx"] }           optional: only these platforms' slugs
 *
 * It needs a current RomM: the game list as pages with each ROM's files and their
 * categories (a file of none is the game; manuals, patches, DLC and the like stay on the
 * server), and a file downloaded by its own id (/api/roms/<file id>/files/content/<name>),
 * which goes on from where it was (Range).
 */
#pragma once

#include "../source.h"

#include <memory>
#include <string>
#include <vector>

namespace ps5::remote::romm
{
std::unique_ptr<Source> make_source(const Json &settings, std::string *error);
} // namespace ps5::remote::romm
