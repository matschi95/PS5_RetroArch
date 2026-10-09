/* PS5 RetroArch - the download sources' games in the library every frontend shows.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RetroArch and EmulationStation show RetroArch's playlists (src/ps5_library.h), so the
 * sources' games (src/remote/remote.h) go into playlists too. They are written as
 * eboot.bin starts (sync), before a frontend has read any and before RetroArch could
 * write them, so RetroArch's own playlist management never meets a change of these:
 *
 *   - a playlist for each platform the sources have games of that the title has a core
 *     for, "<database> (Remote).lpl", each title in it once, labelled with the marker
 *     (sources.json's "marker", " [Remote]" when it names none). Its entries are stubs,
 *     <system folder>/.remote/<game>.remote, run by the remote core (cores/remote/,
 *     PS5_LIBRARY_FETCH_CORE), which downloads the game and has game mode run it. A title
 *     a game on the console is (remote.h, same_as_local) is left out. The system folder,
 *     where the platform's games go, is the folder in content/ named after the platform
 *     ("SNES", "PS1"), else content/<platform>;
 *   - each title's cover in the shared media library (PS5_LIBRARY_MEDIA), under its
 *     stub's name;
 *   - "Remote.lpl": the remote core's own screens (Downloads), so both frontends reach
 *     them;
 *   - a game downloaded since the last start goes to the end of its system's own playlist
 *     (the first, by name, whose name is the platform's, else a new one named after its
 *     database), the rest of that file as it was; one deleted from the console leaves it.
 *
 * What was written is listed in config/remote/written.json, and only that is ever
 * replaced or removed.
 */
#pragma once

#include "remote.h"

#include <string>

namespace ps5::remote
{
/* As eboot.bin starts, before a frontend: what happened to games since (downloaded,
 * deleted) goes into the playlists, and the sources' playlists are written from their
 * kept lists. */
void sync(const Paths &paths);
/* A game was put in its place: its system's playlist takes it at the next start. */
void note_placed(const Paths &paths, const Placed &placed);
/* A game was deleted from the console: its entry leaves its playlist at the next start. */
void note_removed(const Paths &paths, const std::string &platform, const std::string &launch);
/* What a stub says: the title's games on their sources (source key, id), its platform, and
 * the core its platform runs with. False when the file is none. */
struct Stub
{
    std::string platform, core, name;
    std::vector<std::pair<std::string, std::string>> games;
    std::string screen; /* a screen of the remote core's own ("downloads"), not a game */
};
bool read_stub(const std::string &path, Stub *stub);
} // namespace ps5::remote
