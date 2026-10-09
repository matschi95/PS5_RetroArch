/* PS5 RetroArch - a game's save data kept in step with the save store (src/remote/save_sync.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "save_sync.h"

#include "backends.h"
#include "files.h"
#include "remote.h"

#include <utils/md5.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <set>
#include <utility>

namespace ps5::remote
{
namespace
{
/* The backups of a file replaced on the console that are kept. */
constexpr size_t kept_backups = 3;

std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

/* What each file was when the console last had it in step with the store: the MD5 of the
 * console's and the version of the store's copy. It is the store's, as the server and user it
 * is (another one, paired since, knows nothing of it). <store>/synced.json:
 *   {"server": "<address> <user>",
 *    "files": {"<emulator>/<name>": {"console": "<MD5>", "store": "<version>",
 *                                    "game": "<the store's id of the game>"}}}
 * The store's game it was in step with counts too: the same file of another game of the store
 * (found first once the store's list changed) knows nothing of it either. */
struct InStep
{
    std::string console;
    std::string store;
    std::string game;
};

Json synced_file(const std::string &folder, const std::string &server)
{
    std::string content;
    Json synced;
    if (!files::read(folder + "/synced.json", &content) || !Json::parse(content, &synced) ||
        synced.kind != Json::object || text(synced, "server") != server ||
        synced["files"].kind != Json::object)
    {
        synced = Json::record();
        synced.set("server", Json::of(server));
        synced.set("files", Json::record());
    }
    return synced;
}

InStep synced_of(const std::string &folder, const std::string &server, const std::string &key)
{
    const Json synced = synced_file(folder, server);
    const Json &entry = synced["files"][key];
    if (entry.kind != Json::object)
        return {};
    return {text(entry, "console"), text(entry, "store"), text(entry, "game")};
}

void keep_synced(const std::string &folder, const std::string &server, const std::string &key,
                 const InStep &now)
{
    Json synced = synced_file(folder, server);
    Json entry = Json::record();
    entry.set("console", Json::of(now.console));
    entry.set("store", Json::of(now.store));
    entry.set("game", Json::of(now.game));
    if (synced["files"][key] == entry)
        return;
    Json files_ = synced["files"];
    files_.set(key, entry);
    synced.set("files", files_);
    files::make_folders(folder);
    (void)files::write(folder + "/synced.json", synced.write(true));
}

/* A file moved, or copied when it cannot be (another drive). */
bool move_file(const std::string &from, const std::string &to)
{
    if (std::rename(from.c_str(), to.c_str()) == 0)
        return true;
    std::string data;
    if (!files::read(from, &data, 512u << 20) || !files::write(to, data))
        return false;
    std::remove(from.c_str());
    return true;
}

/* The store's copy in place of the console's file; the console's goes to the backups first and
 * comes back when the new one cannot be put in place. */
bool replace(const std::string &fetched, const std::string &path, const std::string &backups,
             std::string *error)
{
    const bool had = files::size(path) >= 0;
    std::string backup;
    if (had)
    {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &utc);
        /* A backup of the same second keeps its name: this one gets another. */
        backup = backups + "/" + stamp;
        for (int n = 2; files::size(backup) >= 0; ++n)
            backup = backups + "/" + stamp + "-" + std::to_string(n);
        if (!files::make_folders(backups) || !move_file(path, backup))
        {
            *error = "Cannot move the console's file to " + backup;
            return false;
        }
        /* The newest few stay, by when they were made (not by their names: the console's clock
         * may have been set back since). This one is the newest whatever its name. */
        std::vector<std::pair<int64_t, std::string>> kept;
        for (const std::string &name : files::names(backups))
            if (backups + "/" + name != backup)
                kept.emplace_back(files::modified(backups + "/" + name), name);
        std::sort(kept.begin(), kept.end());
        for (size_t index = 0; index + kept_backups < kept.size() + 1; ++index)
            files::remove_tree(backups + "/" + kept[index].second);
    }
    if (!files::make_folders(files::parent(path)) || !move_file(fetched, path))
    {
        if (had)
            move_file(backup, path);
        *error = "Cannot put the file in place: " + path;
        return false;
    }
    return true;
}

/* A file of the console's as the store compares it. */
LocalSave local_of(SaveKind kind, const std::string &path, const std::string &emulator,
                   std::string *error)
{
    LocalSave local;
    local.kind = kind;
    local.name = files::base_name(path);
    local.emulator = emulator;
    const int64_t size = files::size(path);
    if (size < 0)
        return local;
    if (!md5_file(path, &local.hash))
    {
        *error = "Cannot read " + path;
        return local;
    }
    local.present = true;
    local.size = uint64_t(size);
    local.updated = files::modified(path);
    return local;
}

/* One file of the game's: what the store and the console have of it, the newer one to the other
 * side. */
SyncFile sync_file(SaveStore &store, const SourceGame &found, const LocalSave &local_,
                   const std::string &path, const std::string &server, const SyncPlaces &places,
                   const SyncChooser &choose)
{
    SyncFile result;
    result.local = local_;
    const LocalSave &local = result.local;
    std::string error;
    const auto failed = [&](const std::string &why)
    {
        result.outcome = SyncOutcome::failed;
        result.message = why;
        return result;
    };
    const std::string key = local.emulator + "/" + local.name;
    const std::string fetched = places.work + "/" + files::safe_name(local.name) + ".store";
    files::remove_tree(fetched);

    SavePlan plan;
    if (!store.compare(found, local, &plan, &error))
        return failed(error);
    /* What changed since this console's last sync, by the save data itself: the console's (its
     * MD5 is not the one it last had in step) and the store's (its version is not that one). */
    InStep synced = synced_of(places.store, server, key);
    if (!synced.game.empty() && synced.game != found.id)
        synced = {};
    /* Both have it and nothing says whether it is the same: a store that keeps no hash (RomM's
     * save states) is asked for its copy once, to compare it. */
    if (local.present && !plan.remote.empty() && plan.hash.empty() && synced.console.empty())
    {
        if (!store.download(found, local, plan, fetched, &error) || !md5_file(fetched, &plan.hash))
            return failed(error.empty() ? "Cannot read the server's copy" : error);
        if (plan.hash == local.hash)
            plan.action = SaveAction::none;
    }
    bool overwrite = false;
    if (local.present && !plan.version.empty() && plan.action != SaveAction::none)
    {
        const bool console_changed = local.hash != synced.console;
        const bool store_changed = plan.version != synced.store;
        if ((!plan.hash.empty() && local.hash == plan.hash) || (!console_changed && !store_changed))
            plan.action = SaveAction::none;
        else if (console_changed && !store_changed)
        {
            plan.action = SaveAction::upload; /* over the copy this console had: nothing lost */
            overwrite = true;
        }
        else if (!console_changed && store_changed)
            plan.action = SaveAction::download;
        else
            plan.action = SaveAction::conflict;
    }
    else if (plan.action == SaveAction::download && local.present && local.hash != synced.console)
        plan.action = SaveAction::conflict; /* changed here: it is not replaced unasked */
    else if (plan.action == SaveAction::upload && plan.remote.empty())
        overwrite = true; /* the store has none: nothing to lose */

    /* A conflict: the player chooses; nothing is overwritten without that. */
    SaveAction action = plan.action;
    bool kept = false;
    const auto resolve = [&]
    {
        const SyncChoice choice = choose ? choose(plan, local) : SyncChoice::neither;
        kept = choice == SyncChoice::neither;
        action = kept                            ? SaveAction::none
                 : choice == SyncChoice::console ? SaveAction::upload
                                                 : SaveAction::download;
        overwrite = choice == SyncChoice::console;
    };
    if (action == SaveAction::conflict)
        resolve();
    /* A plan to send what the console does not have is no plan. */
    if (action == SaveAction::upload && !local.present)
        action = SaveAction::none;

    result.outcome = kept ? SyncOutcome::kept : SyncOutcome::same;
    bool done = true;
    InStep now;
    if (action == SaveAction::upload)
    {
        bool newer = false;
        std::string version;
        done = store.upload(found, local, path, overwrite, &newer, &version, &error);
        result.outcome = SyncOutcome::uploaded;
        /* The store found a newer copy only now: a conflict after all. */
        if (!done && newer && !overwrite)
        {
            plan.action = SaveAction::conflict;
            resolve();
            done = true;
            result.outcome = kept ? SyncOutcome::kept : SyncOutcome::same;
            if (action == SaveAction::upload)
            {
                done = store.upload(found, local, path, true, &newer, &version, &error);
                result.outcome = SyncOutcome::uploaded;
            }
        }
        if (action == SaveAction::upload && done)
            now = {local.hash, version, {}};
    }
    if (result.outcome == SyncOutcome::same && local.present && !plan.version.empty() &&
        plan.action == SaveAction::none)
        now = {local.hash, plan.version, {}};
    if (action == SaveAction::download)
    {
        /* Fetched for the comparison already, or now. */
        done = (files::size(fetched) >= 0 || store.download(found, local, plan, fetched, &error)) &&
               replace(fetched, path,
                       places.backups + "/" + local.emulator + "/" + files::safe_name(local.name),
                       &error);
        if (done && md5_file(path, &now.console))
            now.store = plan.version;
        result.outcome = SyncOutcome::downloaded;
    }
    files::remove_tree(fetched);
    now.game = found.id;
    if (done && !now.console.empty() && !now.store.empty())
        keep_synced(places.store, server, key, now);
    if (!done)
        return failed(error);
    return result;
}

SyncResult failed_result(const std::string &message, const std::string &user = {})
{
    SyncResult result;
    result.outcome = SyncOutcome::failed;
    result.message = message;
    result.user = user;
    return result;
}
} // namespace

SyncResult sync_save_data(const std::string &type, const Json &settings, const SyncGame &game,
                          const SyncPlaces &places, const SyncChooser &choose,
                          const Stopped &stopped)
{
    if (game.save.empty() || game.emulator.empty())
        return failed_result("The game's save file is not known");
    std::string error;
    std::unique_ptr<SaveStore> store = make_save_store(type, settings, places.store, &error);
    if (!store)
        return failed_result(error);
    if (!store->prepare(stopped, &error))
    {
        SyncResult result = failed_result(error);
        result.too_old = store->too_old(&result.version, &result.needed);
        return result;
    }
    const std::string user = store->user();

    /* The game on the store: as a download source's game is found on the console. */
    std::vector<SourceGame> games;
    if (!store->games(&games, stopped, &error))
        return failed_result(error, user);
    const std::string normal = normal_name(game.name);
    const SourceGame *found = nullptr;
    for (const SourceGame &candidate : games)
    {
        Game made;
        if (as_game({}, candidate, &made) &&
            (game.platform.empty() || made.platform == game.platform) &&
            same_as_local(made, game.crc32, normal, game.content))
        {
            found = &candidate;
            break;
        }
    }
    SyncResult result;
    result.user = user;
    if (found == nullptr)
    {
        result.outcome = SyncOutcome::no_game;
        return result;
    }

    /* The files: the save, and each save state the console or the store has. */
    std::vector<std::pair<LocalSave, std::string>> wanted;
    wanted.emplace_back(local_of(SaveKind::save, game.save, game.emulator, &error), game.save);
    if (places.states && !game.state.empty())
    {
        const std::string folder = files::parent(game.state);
        const std::string base = files::base_name(game.state);
        std::set<std::string> names;
        for (const std::string &name : files::names(folder))
            if (is_state_of(name, base))
                names.insert(name);
        std::vector<std::string> theirs;
        if (!store->names(*found, SaveKind::state, game.emulator, &theirs, &error))
        {
            store->finish(false);
            return failed_result(error, user);
        }
        for (const std::string &name : theirs)
            if (is_state_of(name, base))
                names.insert(name);
        for (const std::string &name : names)
            wanted.emplace_back(
                local_of(SaveKind::state, folder + "/" + name, game.emulator, &error),
                folder + "/" + name);
    }
    if (!error.empty())
    {
        store->finish(false);
        return failed_result(error, user);
    }
    files::make_folders(places.work);
    const std::string server = store->address() + " " + user;
    bool all = true, any_kept = false, any_down = false, any_up = false;
    for (const auto &file : wanted)
    {
        if (stopped && stopped())
        {
            all = false;
            result.message = "Stopped";
            break;
        }
        SyncFile done = sync_file(*store, *found, file.first, file.second, server, places, choose);
        all = all && done.outcome != SyncOutcome::failed;
        any_kept = any_kept || done.outcome == SyncOutcome::kept;
        any_down = any_down || done.outcome == SyncOutcome::downloaded;
        any_up = any_up || done.outcome == SyncOutcome::uploaded;
        if (done.outcome == SyncOutcome::failed && result.message.empty())
            result.message = done.message;
        result.files.push_back(std::move(done));
    }
    store->finish(all);
    result.outcome = !all       ? SyncOutcome::failed
                     : any_kept ? SyncOutcome::kept
                     : any_down ? SyncOutcome::downloaded
                     : any_up   ? SyncOutcome::uploaded
                                : SyncOutcome::same;
    return result;
}

std::string emulator_of(const std::string &core_path)
{
    std::string name = files::base_name(core_path);
    const size_t dot = name.find('.');
    if (dot != std::string::npos)
        name.resize(dot);
    static const std::string suffix = "_libretro";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    return name;
}

bool is_state_of(const std::string &name, const std::string &base)
{
    if (base.empty() || name.compare(0, base.size(), base) != 0)
        return false;
    const std::string rest = name.substr(base.size());
    if (rest.empty() || rest == ".auto")
        return true;
    return rest.size() <= 3 &&
           std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool md5_file(const std::string &path, std::string *hash)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    MD5_CTX context;
    MD5_Init(&context);
    std::vector<char> buffer(1u << 16);
    size_t got;
    while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
        MD5_Update(&context, buffer.data(), (unsigned long)got);
    const bool read = !std::ferror(file);
    std::fclose(file);
    if (!read)
        return false;
    unsigned char digest[16];
    MD5_Final(digest, &context);
    static const char hex[] = "0123456789abcdef";
    hash->clear();
    for (unsigned char byte : digest)
    {
        *hash += hex[byte >> 4];
        *hash += hex[byte & 15];
    }
    return true;
}
} // namespace ps5::remote
