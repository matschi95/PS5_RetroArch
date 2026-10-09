/* PS5 RetroArch - a RomM server set up from the WebUI (src/remote/servers.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The address is checked by RomM's heartbeat (it answers without a sign-in, with its
 * version). The account's name and password sign in once, to make the console a client API
 * token of its own (POST /api/client-tokens, named "PS5 RetroArch <console id>", with every
 * scope a sign-in of this console asks for: romm_client.h, console_scopes); the console's
 * token of an earlier setup goes first, as RomM keeps only a few per user. The account needs
 * those scopes itself (RomM's role "editor" has them).
 */
#pragma once

#include "../servers.h"

namespace ps5::remote::romm
{
extern const ServerSignIn sign_in;
} // namespace ps5::remote::romm
