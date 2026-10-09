/* PS5 RetroArch - the download sources' games in the library every frontend shows
 * (src/remote/library.h says what is written when).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "library.h"

#include "../ps5_library.h"
#include "files.h"
#include "save_config.h"

#include <algorithm>
#include <map>
#include <set>
#include <unistd.h>

namespace ps5::remote
{
namespace
{
const std::string remote_suffix = " (Remote).lpl";
const std::string screens_playlist = "Remote.lpl";

std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

/* The library, freed with this: the playlists, and the games copied into content/. */
struct Loaded
{
    struct ps5_library library = {};
    bool ok = false;
    explicit Loaded(const Paths &paths)
    {
        const char *const content[] = {paths.content.c_str(), nullptr};
        ok = ps5_library_load_content(&library, paths.playlists.c_str(), paths.info.c_str(),
                                      paths.cores.c_str(), content) == 0;
    }
    ~Loaded()
    {
        ps5_library_free(&library);
    }
    Loaded(const Loaded &) = delete;
    Loaded &operator=(const Loaded &) = delete;

    std::string core_name(const std::string &path) const
    {
        for (size_t i = 0; i < library.core_count; i++)
            if (path == library.cores[i].path)
                return library.cores[i].name;
        return "";
    }
};

/* The name RetroArch gives a platform's playlist: its database's, else its id. */
std::string playlist_base(const std::string &platform)
{
    const char *database = ps5_library_platform_database(platform.c_str());
    return database ? database : platform;
}

/* "1a2b3c4d" as RetroArch writes a checksum: "1A2B3C4D|crc". */
std::string playlist_crc(const std::string &crc32)
{
    std::string crc = normal_crc(crc32);
    if (crc.empty())
        crc = "00000000";
    for (char &c : crc)
        c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
    return crc + "|crc";
}

Json playlist_entry(const std::string &path, const std::string &label, const std::string &core,
                    const std::string &core_name, const std::string &crc32,
                    const std::string &platform)
{
    Json entry = Json::record();
    entry.set("path", Json::of(path));
    entry.set("label", Json::of(label));
    entry.set("core_path", Json::of(core.empty() ? "DETECT" : core));
    entry.set("core_name", Json::of(core.empty() ? "DETECT" : core_name));
    entry.set("crc32", Json::of(playlist_crc(crc32)));
    entry.set("db_name", Json::of(ps5_library_platform_database(platform.c_str())
                                      ? playlist_base(platform) + ".lpl"
                                      : std::string()));
    return entry;
}

/* A whole playlist, as RetroArch writes one. */
std::string playlist_text(const std::vector<Json> &entries)
{
    Json playlist = Json::record();
    playlist.set("version", Json::of("1.5"));
    playlist.set("default_core_path", Json::of(""));
    playlist.set("default_core_name", Json::of(""));
    playlist.set("label_display_mode", Json::of(0.0));
    playlist.set("right_thumbnail_mode", Json::of(0.0));
    playlist.set("left_thumbnail_mode", Json::of(0.0));
    playlist.set("sort_mode", Json::of(0.0));
    Json &items = playlist.set("items", Json::list());
    for (const auto &entry : entries)
        items.push(entry);
    return playlist.write(true);
}

/* A playlist of the player's, changed: its other fields kept, in their order. */
bool change_playlist(const std::string &file, const std::function<bool(Json &items)> &change)
{
    std::string text_;
    Json playlist;
    if (files::size(file) < 0)
        playlist = Json::parse(playlist_text({}));
    else if (!files::read(file, &text_) || !Json::parse(text_, &playlist) ||
             playlist.kind != Json::object)
        return false; /* not JSON: an old playlist format, left alone */
    Json items = playlist["items"].kind == Json::array ? playlist["items"] : Json::list();
    if (!change(items))
        return true;
    playlist.set("items", std::move(items));
    return files::write(file, playlist.write(true));
}

/* The playlist a platform's games go to: the first, by name, named after the platform. */
std::string system_playlist(const Paths &paths, const std::string &platform)
{
    for (const auto &name : files::names(paths.playlists))
    {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".lpl") != 0 ||
            name == screens_playlist ||
            (name.size() >= remote_suffix.size() &&
             name.compare(name.size() - remote_suffix.size(), remote_suffix.size(),
                          remote_suffix) == 0))
            continue;
        const char *found = ps5_library_platform(name.c_str());
        if (found && platform == found)
            return paths.playlists + "/" + name;
    }
    return paths.playlists + "/" + files::safe_name(playlist_base(platform)) + ".lpl";
}

/* A cover in the shared media library, under a game's key (its file's name). */
std::string media_cover(const Paths &paths, const std::string &platform, const std::string &game,
                        const std::string &cover)
{
    char key[512];
    if (cover.empty() || ps5_library_media_key(game.c_str(), key, sizeof key) != 0)
        return "";
    const size_t dot = cover.find_last_of('.');
    return paths.media + "/" + platform + "/covers/" + key + cover.substr(dot);
}

/* A cover the media library has for that game (any picture type), or "". */
std::string media_cover_there(const Paths &paths, const std::string &platform,
                              const std::string &game)
{
    for (const char *type : {".png", ".jpg"})
        if (const std::string path = media_cover(paths, platform, game, type);
            !path.empty() && files::size(path) > 0)
            return path;
    return "";
}

/* Copies a file unless the copy is there with its size. */
bool copy_file(const std::string &from, const std::string &to)
{
    const int64_t size = files::size(from);
    if (size <= 0)
        return false;
    if (files::size(to) == size)
        return true;
    std::string bytes;
    return files::read(from, &bytes, 64u << 20) && files::make_folders(files::parent(to)) &&
           files::write(to, bytes);
}

/* Writes a file unless it holds that already. */
bool write_changed(const std::string &path, const std::string &text_)
{
    std::string current;
    return (files::read(path, &current) && current == text_) || files::write(path, text_);
}

std::vector<std::string> read_written(const Paths &paths)
{
    std::vector<std::string> written;
    std::string text_;
    Json document;
    if (files::read(paths.config + "/written.json", &text_) && Json::parse(text_, &document))
        for (const auto &file : document["files"].items)
            written.push_back(file.str());
    return written;
}

/* The games on the console of one platform, as same_as_local compares them. */
struct Local
{
    std::string crc32, normal, file;
};

std::map<std::string, std::vector<Local>> games_here(const struct ps5_library &library)
{
    std::map<std::string, std::vector<Local>> here;
    for (size_t i = 0; i < library.game_count; i++)
    {
        const struct ps5_library_game &game = library.games[i];
        const struct ps5_library_system &system = library.systems[game.system];
        const std::string path = game.path;
        if (!system.known || ps5_library_is_fetch_core(game.core) ||
            (path.size() > 7 && path.compare(path.size() - 7, 7, ".remote") == 0))
            continue; /* a source's game, or a game of no platform */
        here[system.id].push_back({game.crc32, normal_name(game.label), path});
    }
    return here;
}

/* The notes left for this start: config/remote/<kind>/<note>.json, each taken by `take`, which
 * says whether it is done with (else it stays for the next start). */
void take_notes(const Paths &paths, const char *kind, const std::function<bool(const Json &)> &take)
{
    const std::string folder = paths.config + "/" + kind;
    for (const auto &name : files::names(folder))
    {
        std::string text_;
        Json note;
        const std::string path = folder + "/" + name;
        if (!files::read(path, &text_) || !Json::parse(text_, &note) || take(note))
            files::remove_tree(path);
    }
}

void leave_note(const Paths &paths, const char *kind, const std::string &key, const Json &note)
{
    const std::string folder = paths.config + "/" + kind;
    if (!files::make_folders(folder) ||
        !files::write(folder + "/" + files::safe_name(key) + ".json", note.write(true)))
        std::fprintf(stderr, "[remote] the note for %s could not be written\n", key.c_str());
}
} // namespace

Paths Paths::title()
{
    return at("/app0");
}

Paths Paths::at(const std::string &app0)
{
    Paths paths;
    paths.config = app0 + "/config/remote";
    paths.content = app0 + "/content";
    paths.downloads = app0 + "/content/.remote-downloads";
    paths.playlists = app0 + "/playlists";
    paths.info = app0 + "/info";
    paths.cores = app0 + "/cores";
    paths.media = app0 + "/library";
    const std::string content = paths.content;
    /* The folder in content/ named after the platform ("SNES", "PS1", "Saturn"), else
     * content/<platform>. */
    paths.system_folder = [content](const std::string &platform)
    {
        for (const auto &name : files::names(content))
        {
            const char *found = ps5_library_platform(name.c_str());
            if (found && platform == found && files::is_folder(content + "/" + name))
                return content + "/" + name;
        }
        return content + "/" + platform;
    };
    return paths;
}

bool read_stub(const std::string &path, Stub *stub)
{
    std::string text_;
    Json document;
    if (!files::read(path, &text_, 1u << 20) || !Json::parse(text_, &document))
        return false;
    *stub = Stub();
    stub->screen = text(document, "screen");
    stub->platform = text(document, "platform");
    stub->core = text(document, "core");
    stub->name = text(document, "name");
    for (const auto &game : document["games"].items)
        stub->games.emplace_back(text(game, "source"), text(game, "id"));
    return !stub->screen.empty() || !stub->games.empty();
}

void note_placed(const Paths &paths, const Placed &placed)
{
    Json note = Json::record();
    note.set("platform", Json::of(placed.game.platform));
    note.set("path", Json::of(placed.launch));
    note.set("label", Json::of(placed.game.name));
    note.set("crc32", Json::of(placed.game.crc32));
    note.set("cover", Json::of(placed.game.cover));
    leave_note(paths, "placed", placed.game.source + "-" + placed.game.id, note);
}

void note_removed(const Paths &paths, const std::string &platform, const std::string &launch)
{
    Json note = Json::record();
    note.set("platform", Json::of(platform));
    note.set("path", Json::of(launch));
    leave_note(paths, "removed", launch, note);
}

void sync(const Paths &paths)
{
    read(paths);
    const Status status = current();
    std::vector<std::string> written_before = read_written(paths);
    /* The save sync set up: its screen is there without download sources too. */
    const bool saves = !read_save_config(paths.config + "/save-sync.json").type.empty();
    if (!status.configured && !saves && written_before.empty() &&
        files::names(paths.config + "/placed").empty() &&
        files::names(paths.config + "/removed").empty())
        return; /* nothing set up, nothing to take back */

    /* What happened since the last start: downloaded games into their system's playlist,
     * deleted ones out of it. */
    {
        Loaded loaded(paths);
        take_notes(
            paths, "placed",
            [&](const Json &note)
            {
                const std::string platform = text(note, "platform"), path = text(note, "path");
                if (files::size(path) < 0 && !files::is_folder(path))
                    return true; /* gone again */
                char core[PS5_LIBRARY_PATH_MAX] = "";
                ps5_library_platform_core(&loaded.library, platform.c_str(), core, sizeof core);
                const Json entry =
                    playlist_entry(path, text(note, "label"), core, loaded.core_name(core),
                                   text(note, "crc32"), platform);
                const bool added = change_playlist(
                    system_playlist(paths, platform),
                    [&](Json &items)
                    {
                        for (const auto &item : items.items)
                            if (text(item, "path") == path)
                                return false; /* there already (scanned in meanwhile) */
                        items.push(entry);
                        return true;
                    });
                if (added && media_cover_there(paths, platform, path).empty())
                    copy_file(text(note, "cover"),
                              media_cover(paths, platform, path, text(note, "cover")));
                return added;
            });
        take_notes(paths, "removed",
                   [&](const Json &note)
                   {
                       const std::string path = text(note, "path");
                       return change_playlist(system_playlist(paths, text(note, "platform")),
                                              [&](Json &items)
                                              {
                                                  Json kept = Json::list();
                                                  for (const auto &item : items.items)
                                                      if (text(item, "path") != path)
                                                          kept.push(item);
                                                  const bool changed =
                                                      kept.items.size() != items.items.size();
                                                  items = std::move(kept);
                                                  return changed;
                                              });
                   });
    }

    Loaded loaded(paths);
    if (!loaded.ok)
        return;
    const std::map<std::string, std::vector<Local>> here = games_here(loaded.library);
    std::string marker = " [Remote]";
    {
        std::string text_;
        Json settings;
        if (files::read(paths.config + "/sources.json", &text_) && Json::parse(text_, &settings) &&
            settings["marker"].kind == Json::string)
            marker = settings["marker"].text;
    }

    /* The titles not on the console, by platform, with a core for it. */
    std::map<std::string, std::string> cores;
    std::map<std::string, std::vector<Title>> listed;
    for (Title &title : titles())
    {
        const Game &first = title.games.front();
        const auto local = here.find(first.platform);
        const bool on_console =
            local != here.end() &&
            std::any_of(title.games.begin(), title.games.end(),
                        [&](const Game &game)
                        {
                            return std::any_of(
                                local->second.begin(), local->second.end(), [&](const Local &l)
                                { return same_as_local(game, l.crc32, l.normal, l.file); });
                        });
        if (on_console)
            continue;
        if (!cores.count(first.platform))
        {
            char core[PS5_LIBRARY_PATH_MAX] = "";
            ps5_library_platform_core(&loaded.library, first.platform.c_str(), core, sizeof core);
            cores[first.platform] = core;
        }
        if (!cores[first.platform].empty())
            listed[first.platform].push_back(std::move(title));
    }

    /* Each platform's stubs, covers and playlist. */
    std::set<std::string> written;
    const std::set<std::string> ours(written_before.begin(), written_before.end());
    const std::string fetch_core = paths.cores + "/" PS5_LIBRARY_FETCH_CORE;
    for (auto &platform : listed)
    {
        std::vector<Title> &list = platform.second;
        std::sort(list.begin(), list.end(), [](const Title &a, const Title &b)
                  { return a.games.front().name < b.games.front().name; });
        const std::string stubs = paths.system_folder(platform.first) + "/.remote";
        files::make_folders(stubs);
        std::vector<Json> entries;
        std::set<std::string> names;
        for (const Title &title : list)
        {
            const Game &game = title.games.front();
            const std::string base = files::safe_name(game.name);
            std::string name = base;
            for (int n = 2; names.count(name); n++)
                name = base + " (" + std::to_string(n) + ")";
            names.insert(name);
            Json stub = Json::record();
            stub.set("platform", Json::of(platform.first));
            stub.set("core", Json::of(cores[platform.first]));
            stub.set("name", Json::of(game.name));
            Json &games_ = stub.set("games", Json::list());
            std::string cover;
            for (const Game &each : title.games)
            {
                Json &entry = games_.push(Json::record());
                entry.set("source", Json::of(each.source));
                entry.set("id", Json::of(each.id));
                if (cover.empty())
                    cover = each.cover; /* the first source's, or another one's */
            }
            const std::string path = stubs + "/" + name + ".remote";
            write_changed(path, stub.write(true));
            written.insert(path);
            /* Into the media library only where no cover of the player's is: one of the same
             * name stays, and is neither written over nor deleted later. */
            const std::string there = media_cover_there(paths, platform.first, path);
            if (const std::string media = media_cover(paths, platform.first, path, cover);
                !media.empty() && (there.empty() || (there == media && ours.count(media))) &&
                copy_file(cover, media))
                written.insert(media);
            entries.push_back(playlist_entry(path, game.name + marker, fetch_core, "Remote",
                                             game.crc32, platform.first));
        }
        const std::string playlist =
            paths.playlists + "/" + files::safe_name(playlist_base(platform.first)) + remote_suffix;
        write_changed(playlist, playlist_text(entries));
        written.insert(playlist);
    }

    /* The remote core's own screens: Downloads with download sources, Save sync with those
     * (to pair with one of them) or a server of the save sync. */
    std::vector<std::pair<const char *, const char *>> own;
    if (status.configured)
        own.emplace_back("downloads", "Downloads");
    if (status.configured || saves)
        own.emplace_back("savesync", "Save sync");
    if (!own.empty())
    {
        const std::string screens = paths.config + "/screens";
        files::make_folders(screens);
        std::vector<Json> entries;
        for (const auto &screen : own)
        {
            Json stub = Json::record();
            stub.set("screen", Json::of(screen.first));
            const std::string path = screens + "/" + screen.second + ".remote";
            write_changed(path, stub.write(true));
            written.insert(path);
            entries.push_back(playlist_entry(path, screen.second, fetch_core, "Remote", "", ""));
        }
        const std::string playlist = paths.playlists + "/" + screens_playlist;
        write_changed(playlist, playlist_text(entries));
        written.insert(playlist);
    }

    /* What was written before and is not now; a cover that is now a game's on the console (a
     * downloaded one under the same name) stays. */
    std::set<std::string> kept_covers;
    for (const auto &platform : here)
        for (const Local &local : platform.second)
            if (const std::string cover = media_cover_there(paths, platform.first, local.file);
                !cover.empty())
                kept_covers.insert(cover);
    for (const auto &path : written_before)
        if (!written.count(path) && !kept_covers.count(path))
        {
            files::remove_tree(path);
            if (files::base_name(files::parent(path)) == ".remote")
                rmdir(files::parent(path).c_str()); /* when it is empty */
        }
    Json document = Json::record();
    Json &list = document.set("files", Json::list());
    for (const auto &path : written)
        list.push(Json::of(path));
    files::make_folders(paths.config);
    write_changed(paths.config + "/written.json", document.write(true));
}
} // namespace ps5::remote
