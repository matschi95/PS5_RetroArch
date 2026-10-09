/* PS5 RetroArch - the RomM backend of the download sources' firmware (src/remote/firmware.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RomM keeps firmware by platform (its library's bios/<platform>/ folders, or uploaded in its
 * web page): /api/firmware lists all of it, each file with its platform's id and its hashes,
 * and /api/firmware/<id>/content/<name> hands one out. The entry is the download source's
 * (romm_source.h); a client API token needs the scope firmware.read for it.
 */
#pragma once

#include "../firmware.h"

#include <memory>
#include <string>

namespace ps5::remote::romm
{
std::unique_ptr<FirmwareSource> make_firmware(const Json &settings, std::string *error);
} // namespace ps5::remote::romm
