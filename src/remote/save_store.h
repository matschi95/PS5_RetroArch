/* PS5 RetroArch - a save store: a place that keeps the save data of the console's games
 * besides the console, file by file: a game's save (its .srm) and its save states.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each kind of server that can keep save data is a backend that implements this interface
 * (backends.h); save_sync.h decides what to do with a game's save data, the same way for every
 * backend, and does it through this. A backend only talks to its server.
 *
 * A store is made for the entry in config/remote/save-sync.json (save_config.h), with a folder
 * of its own on the console for what it keeps between starts (the device it is to the server,
 * for example).
 */
#pragma once

#include "source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ps5::remote
{
/* What a file of a game's save data is. */
enum class SaveKind : uint8_t
{
    save,  /* the game's own (its battery save, memory card...: RetroArch's .srm) */
    state, /* a save state of RetroArch's (.state, .state1..., .state.auto) */
};

/* A file of a game's save data on the console, as the store compares it with its copy. */
struct LocalSave
{
    SaveKind kind = SaveKind::save;
    std::string name;     /* its file name, the same on the store ("Chrono Trigger.srm") */
    std::string emulator; /* the core that made it, as the store keeps them apart ("snes9x") */
    bool present = false; /* false: the console has no such file */
    std::string hash;     /* the MD5 of its contents, 32 lower-case hex digits */
    int64_t updated = 0;  /* when it was last changed, seconds since 1970 (UTC) */
    uint64_t size = 0;
};

enum class SaveAction : uint8_t
{
    none,     /* both have the same, or neither has any */
    upload,   /* the console's is newer, or the store has none */
    download, /* the store's is newer, or the console has none */
    conflict, /* both changed since they were last the same: the player chooses */
};

/* What the store says about a file of a game's save data. */
struct SavePlan
{
    SaveAction action = SaveAction::none;
    std::string reason;  /* why, in English, for the log */
    std::string remote;  /* the store's own name for its copy; empty without one */
    std::string version; /* what changes whenever its copy does; empty without one */
    std::string hash;    /* the MD5 of its contents; empty when the store does not say */
    int64_t updated = 0; /* when its copy was made, seconds since 1970 (UTC) */
    std::string device;  /* what made it ("Steam Deck"); empty when not known */
    uint64_t size = 0;
};

class SaveStore
{
  public:
    virtual ~SaveStore() = default;
    /* Where it is, as the player reads it ("http://nas:3000"). */
    virtual std::string address() const = 0;
    /* Signs in and gets ready for a sync. Blocking (the network); false with *error. */
    virtual bool prepare(const Stopped &stopped, std::string *error) = 0;
    /* Who it is signed in as ("alex"), once prepared; empty when the server does not say. */
    virtual std::string user() const = 0;
    /* After prepare() failed: whether it was because the server is older than the backend
     * takes, with the server's version and the oldest one taken. */
    virtual bool too_old(std::string *version, std::string *needed) const
    {
        (void)version;
        (void)needed;
        return false;
    }
    /* The games it can keep save data of, as a download source lists its games. */
    virtual bool games(std::vector<SourceGame> *games, const Stopped &stopped,
                       std::string *error) = 0;
    /* The names of the files of a kind it has of a game (game: one of games()), so that one
     * the console does not have comes too. */
    virtual bool names(const SourceGame &game, SaveKind kind, const std::string &emulator,
                       std::vector<std::string> *names, std::string *error) = 0;
    /* What to do with a file of a game's save data. */
    virtual bool compare(const SourceGame &game, const LocalSave &local, SavePlan *plan,
                         std::string *error) = 0;
    /* Keeps the file `path` as that file of the game. overwrite: also when the store's copy
     * changed since this console last had it (the player chose the console's in a conflict).
     * Without it, such a copy refuses the upload: false with *newer set (a conflict after all;
     * a store may only find it here). *version: its copy's version now. */
    virtual bool upload(const SourceGame &game, const LocalSave &local, const std::string &path,
                        bool overwrite, bool *newer, std::string *version, std::string *error) = 0;
    /* The store's copy (plan: from compare()) into the file `path`. */
    virtual bool download(const SourceGame &game, const LocalSave &local, const SavePlan &plan,
                          const std::string &path, std::string *error) = 0;
    /* A sync of a game is over: done tells whether all of it worked. */
    virtual void finish(bool done) = 0;
};
} // namespace ps5::remote
