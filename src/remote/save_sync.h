/* PS5 RetroArch - a game's save data kept in step with the save store (save_store.h), the same
 * way for every backend.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each file of a game's save data (its save, its save states) on its own: the store says
 * whether its copy or the console's is newer, and the newer one goes to the other side. When
 * both changed since they were last the same (played on two devices without a sync between),
 * the player chooses; nothing is overwritten without that. Which side changed is told by the
 * save data itself: the console keeps what each file was when it was last in step, on both
 * sides (<store>/synced.json), so a store that only compares times does not replace save data
 * changed on the console. A file replaced on the console goes to a backup folder first (the
 * last few of each file are kept).
 *
 * Blocking (the network), and on no thread of its own: the caller runs it where waiting is
 * fine.
 */
#pragma once

#include "save_store.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps5::remote
{
/* A game on the console whose save data syncs, as RetroArch has it when it loads it. */
struct SyncGame
{
    std::string platform; /* "snes" (ps5_library.h); empty: not known, any is taken */
    std::string name;     /* as its playlist shows it */
    std::string content;  /* the file the core runs */
    std::string crc32;    /* of that file, as its playlist has it; empty when not known */
    std::string emulator; /* the core: "snes9x" of snes9x_libretro.so */
    std::string save;     /* its save file (RetroArch's name.savefile) */
    std::string state;    /* its save states without their slot (name.savestate) */
};

struct SyncPlaces
{
    std::string store;   /* the store's folder for what it keeps between starts */
    std::string work;    /* a folder for the files while they are needed */
    std::string backups; /* where replaced files go (<backups>/<emulator>/<name>/<time>) */
    bool states = true;  /* the save states sync too */
};

/* The player's choice in a conflict. */
enum class SyncChoice : uint8_t
{
    console,
    server,
    neither
};
/* Asked in a conflict: what the store has (plan) and what the console has (local). */
using SyncChooser = std::function<SyncChoice(const SavePlan &plan, const LocalSave &local)>;

enum class SyncOutcome : uint8_t
{
    same,       /* nothing to do */
    uploaded,   /* the console's went to the store */
    downloaded, /* the store's replaced the console's */
    kept,       /* a conflict the player left as it is */
    no_game,    /* the store has no such game: it cannot keep its save data */
    failed,     /* see message */
};

/* A file of the game's save data and what became of it. */
struct SyncFile
{
    LocalSave local;
    SyncOutcome outcome = SyncOutcome::same;
    std::string message; /* failed: why */
};

struct SyncResult
{
    /* The whole game's: failed when a file failed, else kept when a conflict was left, else
     * downloaded or uploaded when a file went, else same (or no_game). */
    SyncOutcome outcome = SyncOutcome::failed;
    std::string message; /* what went wrong, in English; empty otherwise */
    std::string user;    /* who the store is signed in as, when it says */
    /* Failed because the server is older than its backend takes: its version and the oldest. */
    bool too_old = false;
    std::string version, needed;
    std::vector<SyncFile> files;
};

/* Syncs a game's save data with the store of `type` made from `settings` (the entry in
 * save-sync.json). choose: asked in a conflict; nullptr leaves a conflict as it is. */
SyncResult sync_save_data(const std::string &type, const Json &settings, const SyncGame &game,
                          const SyncPlaces &places, const SyncChooser &choose,
                          const Stopped &stopped);

/* ---- the parts, for tests ---- */
/* The core's name a save is kept under: "snes9x" of ".../snes9x_libretro.so". */
std::string emulator_of(const std::string &core_path);
/* Whether `name` is one of the save states of `base` ("Game.state"): base itself, a slot
 * ("Game.state3") or the automatic one ("Game.state.auto"). */
bool is_state_of(const std::string &name, const std::string &base);
/* The MD5 of a file's contents, 32 lower-case hex digits; false when it cannot be read. */
bool md5_file(const std::string &path, std::string *hash);
} // namespace ps5::remote
