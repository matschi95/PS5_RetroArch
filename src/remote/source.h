/* PS5 RetroArch - a download source: a place that has games and hands them out file by file.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each kind of source (a kind of game server) is a backend that implements this
 * interface; src/remote/remote.h lists the games of every source set up, downloads them
 * and keeps what it needs between starts, the same way for every backend. A backend
 * only talks to its server.
 *
 * A new backend: a class deriving from Source in a folder of its own
 * (src/remote/<type>/), and the function that makes it in make_source
 * (src/remote/backends.cpp), for its "type" in sources.json.
 */
#pragma once

#include "../scraper_json.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ps5::remote
{
using Json = ps5_scraper::Json;

/* What a file of a game is: the game itself, or its updates and DLC (which RetroArch's
 * cores do not take: they stay on the server). */
enum class FileKind : uint8_t
{
    game,
    update,
    dlc
};

struct SourceFile
{
    std::string id;    /* the source's own name for it (it fetches it by this) */
    std::string name;  /* its path inside the game's folder ("Disc 1.cue", "Disc 1/Track 01.bin") */
    uint64_t size = 0; /* 0 when the source does not know */
    FileKind kind = FileKind::game;
    /* What the source knows of its contents, as hex digits; empty when it does not. */
    std::string crc32, md5, sha1;
};

struct SourceGame
{
    std::string id;   /* the source's own name for the game */
    std::string name; /* as the source names it */
    /* What the source calls its system, most precise first ("snes", "Super Nintendo"):
     * the generic part tells the platform from them (src/ps5_library.h). */
    std::vector<std::string> systems;
    std::string folder; /* the name of its folder on the source: its files go into one so named */
    /* What tells the game apart on any source, as far as this one knows (remote.h,
     * same_game): its files' checksums (above), its ids at metadata providers ("igdb" ->
     * "1234"; lower-case provider names), and whether its name comes from such metadata,
     * not from a file name. */
    std::map<std::string, std::string> ids;
    bool identified = false;
    /* The source's own name for its cover, which changes when the picture does; empty
     * without one. */
    std::string cover;
    std::vector<SourceFile> files; /* its files the console can use */
};

/* Where a download's bytes go (src/remote/remote.cpp). */
class Receiver
{
  public:
    virtual ~Receiver() = default;
    /* Before the first byte: from_start when the file comes from its beginning, not from
     * the offset asked for (a source that cannot go on). False stops the transfer. */
    virtual bool begin(bool from_start) = 0;
    /* The next bytes; false stops the transfer. */
    virtual bool take(const void *data, size_t size) = 0;
    /* Asked often while the transfer runs: true stops it (a game starts, a cancel). */
    virtual bool stopped() = 0;
};

/* Asked often during a list or a cover: true stops it. */
using Stopped = std::function<bool()>;

class Source
{
  public:
    virtual ~Source() = default;
    /* Where it is, as the player reads it ("http://nas:3000"). */
    virtual std::string address() const = 0;
    /* All of its games. Blocking (the network); false with *error, in English. timeout: the
     * seconds to wait for each answer (0: the HTTP client's). */
    virtual bool list(std::vector<SourceGame> *games, std::string *error, const Stopped &stopped,
                      unsigned timeout = 0) = 0;
    /* A game's cover as a picture file's bytes (PNG or JPEG); false when there is none. */
    virtual bool cover(const SourceGame &game, std::string *picture, const Stopped &stopped) = 0;
    /* A file of a game from `offset` on, into the receiver (or from its start:
     * Receiver::begin). True when all of it came; false with *error otherwise, or when
     * the receiver stopped it. */
    virtual bool fetch(const SourceGame &game, const SourceFile &file, uint64_t offset,
                       Receiver &receiver, std::string *error) = 0;
};

/* Makes the source of a "type" from its entry in sources.json. nullptr with *error when
 * the type is unknown or the entry is not usable. */
std::unique_ptr<Source> make_source(const std::string &type, const Json &settings,
                                    std::string *error);
} // namespace ps5::remote
