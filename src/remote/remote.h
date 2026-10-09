/* PS5 RetroArch - download sources: places on the network that have games (source.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Their games are listed beside the console's own (src/remote/library.h writes them
 * into RetroArch's playlists, which both frontends read). A game is downloaded to the
 * content folder when it is played or put in the download queue; nothing is streamed:
 * it runs from its files on the console, as one copied there by hand. The same game on
 * several sources is one title that can be downloaded from any of them (same_game
 * tells), and a game already on the console is not offered.
 *
 * The sources are set up in config/remote/sources.json, which the player writes (over
 * FTP); each entry names its backend's "type" (src/remote/backends.cpp),
 * an optional "name" for the menu, and what the backend needs (see its header,
 * src/remote/<type>/):
 *
 *   { "sources": [ { "type": "<type>", "name": "Home", ... } ] }
 *
 * What is kept, in config/remote/: queue.json (the download queue, one for all sources,
 * so it goes on after the title was closed), <source>/catalog.json (each source's game
 * list, so the games are listed at once and without a network) and <source>/covers/.
 *
 * A download is written to content/.remote-downloads/<source>/<game>/ and, once all of
 * the game's files are complete, moved to its place at once: <system folder>/<game>/,
 * the files as they are on the source, so the system folder only ever holds whole
 * games. One that stopped (the title closed, a game started, the network went away)
 * goes on from where it was, its last 4 MB fetched again in case of a power cut;
 * .remote-downloads/ keeps nothing else (a cancel deletes a game's folder, the next
 * start whatever is not queued). Everything runs on two threads of this file's own;
 * the menu (src/remote/ui.h) only reads their state.
 */
#pragma once

#include "source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ps5::remote
{
struct Paths
{
    std::string config;    /* config/remote: sources.json, queue.json, <source>/ */
    std::string content;   /* where games go (their system's folder in it) */
    std::string downloads; /* content/.remote-downloads, for the files being downloaded */
    /* A platform's folder, where its games go ("/app0/content/SNES"). */
    std::function<std::string(const std::string &platform)> system_folder;
    /* RetroArch's playlists, core info and cores, and the shared media library
     * (src/ps5_library.h): where the games are listed (src/remote/library.h). */
    std::string playlists, info, cores, media;
    /* The title's: /app0/... (the system folder: src/remote/library.h). */
    static Paths title();
    /* The same under another folder than /app0 (the tests'). */
    static Paths at(const std::string &app0);
};

/* A downloaded game was put in its place: where, and the file that is run. */
struct Placed;
using PlacedNote = std::function<void(const Placed &placed)>;

/* One file of a game. */
struct Part
{
    std::string id;   /* the source's name for it */
    std::string name; /* its path inside the game's folder */
    uint64_t size = 0;
    std::string crc32, md5, sha1; /* what the source knows of its contents */
};

/* A game on a source. */
struct Game
{
    std::string source; /* the key of its source (SourceStatus::key) */
    std::string id;     /* the source's name for it */
    std::string name;
    std::string platform;     /* src/ps5_library.c's ("snes") */
    std::string folder;       /* its folder's name in the system folder, once downloaded */
    std::string file;         /* the file that is run (one of parts), inside the folder */
    uint64_t size = 0;        /* all of its files */
    std::string cover;        /* its cover on the console; empty without one */
    std::string cover_source; /* the source's name for the cover */
    std::vector<Part> parts;
    /* What tells it apart (source.h): the checksum of the file that is run (8 lower-case hex
     * digits; empty when unknown), its ids at metadata providers, and whether its name comes
     * from such metadata. */
    std::string crc32;
    std::map<std::string, std::string> ids;
    bool identified = false;
    std::string normal_name; /* normal_name(name), kept for comparing */
};

struct Placed
{
    Game game;
    std::string folder; /* the game's folder on the console */
    std::string launch; /* the file that is run */
};

/* The same game on several sources: one title, which can be downloaded from any of them. */
struct Title
{
    std::string key;         /* which title it is, for the menu (title_key) */
    std::vector<Game> games; /* the game on each source that has it, in sources.json's order */
};

/* ---- which game is which ---- */
/* Two sources' games are the same game when, asked in this order, the first thing both know
 * is the same: their checksum; their id at a metadata provider both have one of; their
 * names, when both come from metadata (normal_name); else their file names. Games of two
 * platforms are never the same. */
bool same_game(const Game &a, const Game &b);
/* A source's game is a game on the console: by the file name it is downloaded as first,
 * then by their checksums when both know theirs, else by their names when the source's
 * comes from metadata. normal_name: the console game's name as normal_name makes it (once,
 * for all the sources' games it is compared with). */
bool same_as_local(const Game &game, const std::string &crc32, const std::string &normal_name,
                   const std::string &file);
/* A source's game as the console takes it (its platform and files); false when it has no
 * platform the console knows or no file it can use. source: the key of its source. */
bool as_game(const std::string &source, const SourceGame &from, Game *game);
/* A name as names are compared: lower case, without accents, marks, tags in brackets
 * ("(USA)", "[!]") and anything but letters and digits ("Pokémon: Let's Go! (Europe)" is
 * "pokemonletsgo"). */
std::string normal_name(const std::string &name);
/* A text as the menu's font draws it: ASCII, the accents off Latin letters ("Pokémon" is
 * "Pokemon"), any other character a '?'. */
std::string plain_text(const std::string &text);
/* "1A2B3C4D", "1a2b3c4d|crc", "0x1A2B3C4D" as "1a2b3c4d"; "" for anything else. */
std::string normal_crc(const std::string &crc);
/* A title's key, from its first game: its checksum, else its first provider id, else its
 * name or file name ("crc:1a2b3c4d", "screenscraper:195863", "name:...", "file:..."). */
std::string title_key(const Game &game);
/* The file of a game that is run, of its files' paths and sizes: an .m3u, .cue, .gdi, .ccd
 * or .chd, the first by name, else the largest. */
std::string launch_file(const std::vector<std::string> &paths, const std::vector<uint64_t> &sizes);

/* verifying: a download that goes on from where it was reads what its file has so far
 * first, for the check of its contents; done goes up to where the download goes on. */
enum class State : uint8_t
{
    queued,
    downloading,
    verifying,
    failed
};
struct Download
{
    std::string source;
    std::string id;
    State state = State::queued;
    uint64_t done = 0;  /* bytes of the whole game on the console so far */
    uint64_t total = 0; /* the whole game; 0 when not known */
    uint64_t rate = 0;  /* bytes a second, smoothed, while it downloads; 0 when not known yet */
    std::string error;  /* why it failed (English) */
};

struct SourceStatus
{
    std::string key;     /* stable, from its name: its folder in config/remote and in the queue */
    std::string name;    /* as the menu shows it */
    std::string address; /* where it is; empty when its entry is not usable */
    bool refreshing = false;
    bool online = false; /* the last look at it worked */
    std::string error;   /* why the last look failed, or what is wrong with its entry */
    size_t games = 0;
};

struct Status
{
    bool configured = false; /* sources.json names at least one source */
    std::string error;       /* what is wrong with sources.json itself */
    std::vector<SourceStatus> sources;
    /* Changes whenever a game list, a cover or a game on the console changed. */
    uint64_t generation = 0;
};

/* Reads sources.json and the kept lists (again: an edited sources.json applies), without
 * the threads: what is listed as eboot.bin starts (src/remote/library.h). */
void read(const Paths &paths);
/* Reads the lists again of the sources that have none yet or one of another address,
 * waiting `timeout` seconds for each answer: as the title starts, so that a source just
 * set up shows its games at once. Blocking. */
void list_new(const Paths &paths, unsigned timeout);
/* read(), the kept queue, and lets the threads run: a source's list is read when it was not
 * yet, or long ago, and the queue goes on. Called whenever RetroArch's menu is up. placed:
 * told of each game put in its place (the playlists take it at the next start). */
void start(const Paths &paths, PlacedNote placed);
/* Stops the threads before a game starts: a download stops (its file stays, for later),
 * nothing is read from the network while the game runs. Waits a moment for the transfer to
 * end. */
void stop();
/* Reads every source's game list again. */
void refresh();
/* Games on the console changed (one was deleted). */
void changed();

Status current();
/* Every source's games, in the order of sources.json. */
std::vector<Game> games();
/* The same, as titles: each game once, with every source that has it. */
std::vector<Title> titles();
/* One game of a source; false when it has no such game. */
bool find(const std::string &source, const std::string &id, Game *game);
std::vector<Download> downloads();

/* Puts a game in the download queue: first (it is played as soon as it is there) or last.
 * A game whose download failed is tried again. False when the source has no such game,
 * or when the same game (same_game) is already queued from another source. */
bool enqueue(const std::string &source, const std::string &id, bool first);
/* Takes a game out of the queue; what it had downloaded is deleted. */
bool cancel(const std::string &source, const std::string &id);
/* The game's folder on the console, when it is there ("" else). */
std::string placed_folder(const std::string &source, const std::string &id);
} // namespace ps5::remote
