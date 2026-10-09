/* PS5 RetroArch - the game servers as the WebUI sets them up: one server for its games, their
 * firmware and the save sync, signed in once.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What is set up stays in the files the title reads (remote.h: sources.json, save_config.h:
 * save-sync.json), so they can still be written by hand; this only writes them. A server is an
 * entry of sources.json, with what it is used for:
 *
 *   { "type": "romm", "name": "Home", "url": "http://nas:3000", "token": "rmm_...",
 *     "games": true,       its games are listed and downloaded (remote.h)
 *     "firmware": true,    the BIOS files a core lacks come from it (firmware.h)
 *     "__server_user": "alex" }   who it is signed in as (the title's, to be read)
 *
 * and its save sync is save-sync.json pointing at the same server and sign-in. The password
 * typed in is used once: the backend makes the console a sign-in of its own (ServerSignIn)
 * and the password is not kept.
 */
#pragma once

#include "source.h"

#include <string>

namespace ps5::remote
{
/* What a server at an address is. */
struct ServerProbe
{
    std::string url;     /* the address as the backend uses it ("http://nas:3000") */
    std::string version; /* the server's version, as it says */
    bool too_old = false;
    std::string needed; /* too_old: the oldest version the backend takes */
};

/* A backend that can be set up from the WebUI has one (backends.h). */
struct ServerSignIn
{
    /* Whether a server of this kind answers at `url`, and which version. False with *error
     * (not reachable, not such a server). */
    bool (*probe)(const std::string &url, ServerProbe *probe, std::string *error);
    /* Signs in with an account's name and password, once, and makes the console a sign-in of
     * its own that can do all a server is used for: `entry` gets the fields the config files
     * need ("url", "token"...) and *user who it is. folder: the save store's
     * (config/remote/save-sync), for what tells this console apart. */
    bool (*sign_in)(const std::string &url, const std::string &name, const std::string &password,
                    const std::string &folder, Json *entry, std::string *user, std::string *error);
};

/* A server as the WebUI's wizard sets it up. */
struct ServerSetup
{
    std::string type = "romm";
    std::string name; /* as the menus show it; the address's host when empty */
    std::string url;
    std::string user, password; /* the account, to sign in anew; empty: the sign-in stays */
    bool games = true, firmware = true, saves = false, states = true;
};

/* The servers set up (config: config/remote), for the WebUI: sources.json's entries, and
 * save-sync.json's server when it is none of them; no token or password. A JSON list of
 * {name, type, url, user, games, firmware, saves, states, signed_in}. */
Json servers_json(const std::string &config);
/* Asks the server at an address what it is (the backend of `type`). */
bool probe_server(const std::string &type, const std::string &url, ServerProbe *probe,
                  std::string *error);
/* Adds a server, or changes the one named `previous`; false with *error, the files then as
 * they were. It is refused when its version is older than its backend takes. */
bool save_server(const std::string &config, const ServerSetup &setup, const std::string &previous,
                 std::string *error);
/* Takes a server out of sources.json, and its save sync when save-sync.json names it. */
bool remove_server(const std::string &config, const std::string &name, std::string *error);
} // namespace ps5::remote
