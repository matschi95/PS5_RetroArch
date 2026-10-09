/* PS5 RetroArch - download sources (src/remote/remote.h says what they are). Nothing here
 * knows a backend: they are made by make_source (src/remote/backends.h) and used through
 * Source (src/remote/source.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "remote.h"

#include "../ps5_library.h"
#include "../title_threads.hpp"
#include "backends.h"
#include "files.h"
#include "stream_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ps5::remote
{
namespace
{
using Clock = std::chrono::steady_clock;

/* A list is read again when the menu is up this long after the last time. */
constexpr auto refresh_age = std::chrono::minutes(15);
/* A download that goes on fetches its last bytes again: after a power cut, the end of a
 * file may not hold what was written there. */
constexpr uint64_t rewind_bytes = 4u << 20;

struct Entry
{
    uint64_t serial = 0; /* tells entries apart for the threads */
    std::string source;
    std::string id;
    State state = State::queued;
    uint64_t done = 0;
    uint64_t total = 0;
    std::string error;
};

struct SourceState
{
    std::string key;
    std::string name;
    std::string signature;          /* its entry in sources.json, to see a change */
    std::shared_ptr<Source> source; /* nullptr when the entry is not usable */
    std::string error;
    bool refresh_wanted = false;
    bool refreshing = false;
    bool online = false;
    bool listed_once = false; /* its list was read since the title started */
    Clock::time_point listed_at{};
    std::vector<Game> games;
};

/* A cancelled game's folder in .remote-downloads/, to be deleted. */
struct Discard
{
    std::string source;
    std::string id;
};

struct Shared
{
    std::mutex lock;
    std::condition_variable wake;
    Paths paths;
    PlacedNote placed;
    bool configured = false;
    std::string error; /* about sources.json */
    std::vector<std::shared_ptr<SourceState>> sources;
    std::deque<Entry> queue;
    /* Deleted by the download thread without the lock (a game of gigabytes takes a while),
     * and before it begins another download: one of the same game finds nothing of the
     * old one. */
    std::vector<Discard> discards;
    /* The covers on the console, known without asking the drive: the menu asks for them
     * often, and a drive busy with a download can keep it waiting. */
    std::set<std::string> covers;
    /* Each source's placed.json, read once. */
    std::map<std::string, Json> placed_games;
    uint64_t serials = 0;
    bool threads = false;
    bool queue_read = false;
    uint64_t generation = 1;
    bool transferring = false; /* the download thread is in a transfer */
    bool listing = false;      /* the list thread is */
    /* Read by the transfers' stop questions, without the lock. */
    std::atomic<bool> halt{true};
    std::atomic<uint64_t> cancel{0};
    std::atomic<uint64_t> current{0};
    std::atomic<uint64_t> current_done{0};
    std::atomic<uint64_t> current_rate{0}; /* bytes a second, smoothed; 0 until a second went by */
};

Shared &state()
{
    static Shared *shared = new Shared; /* lives as long as the process: its threads use it */
    return *shared;
}

void log(const std::string &line)
{
    std::fprintf(stderr, "[remote] %s\n", line.c_str());
}

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

/* A name as one part of a path: letters, digits, '-' and '_' stay, the rest is %XX. */
std::string safe(const std::string &name)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : name)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    return out;
}

/* A file's path inside a game's folder, each of its parts a usable name; "" when it is
 * none (a source's path cannot leave the folder). */
std::string part_path(const std::string &path)
{
    std::string out, part;
    for (size_t at = 0; at <= path.size(); at++)
        if (at == path.size() || path[at] == '/' || path[at] == '\\')
        {
            if (!part.empty() && part != "." && part != "..")
                out += (out.empty() ? "" : "/") + files::safe_name(part);
            part.clear();
        }
        else
            part += path[at];
    return out;
}

/* Unknown on the console: statfs is only in libkernel_sys, which a title does not import,
 * and the SDK's libc.a has it as a raw system call, which ends the process. A full drive
 * still stops the download when a write fails, before anything reaches the game's folder. */
bool free_space(const std::string &, uint64_t *)
{
    return false;
}

std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

/* A source's game as the console takes it: its platform and the game's files (updates and
 * DLC stay on the source), the file that is run chosen among them. */
bool make_game(const std::string &source, const SourceGame &from, Game *game)
{
    *game = Game();
    game->source = source;
    game->id = from.id;
    game->name = from.name;
    game->cover_source = from.cover;
    for (const auto &system : from.systems)
        if (const char *platform = ps5_library_platform(system.c_str()))
        {
            game->platform = platform;
            break;
        }
    for (const auto &id : from.ids)
        if (!id.first.empty() && !id.second.empty())
            game->ids[lower(id.first)] = id.second;
    game->identified = from.identified;
    std::vector<std::string> names;
    std::vector<uint64_t> sizes;
    for (const SourceFile &file : from.files)
    {
        const std::string name = part_path(file.name);
        /* Two files may not end up with one name. */
        if (file.kind != FileKind::game || name.empty() ||
            std::find(names.begin(), names.end(), name) != names.end())
            continue;
        game->parts.push_back({file.id, name, file.size, file.crc32, file.md5, file.sha1});
        names.push_back(name);
        sizes.push_back(file.size);
        game->size += file.size;
    }
    if (game->platform.empty() || game->parts.empty() || from.id.empty())
        return false;
    game->file = launch_file(names, sizes);
    for (const Part &part : game->parts)
        if (part.name == game->file)
            game->crc32 = normal_crc(part.crc32);
    game->folder = files::safe_name(!from.folder.empty() ? from.folder : from.name);
    if (game->name.empty())
        game->name = game->folder;
    game->normal_name = normal_name(game->name);
    return true;
}

/* What a backend needs to fetch one of its games' files. */
SourceGame as_source_game(const Game &game)
{
    SourceGame source;
    source.id = game.id;
    source.name = game.name;
    source.folder = game.folder;
    source.cover = game.cover_source;
    for (const Part &part : game.parts)
        source.files.push_back(
            {part.id, part.name, part.size, FileKind::game, part.crc32, part.md5, part.sha1});
    return source;
}

/* ---- the kept lists and queue ---- */

std::string source_folder(const Shared &s, const std::string &key)
{
    return s.paths.config + "/" + key;
}

/* A cover on the console, in its source's folder: its picture's kind told by its first bytes. */
std::string cover_path(const std::string &folder, const std::string &id, const std::string &picture)
{
    const bool jpeg = picture.size() > 2 && static_cast<unsigned char>(picture[0]) == 0xff &&
                      static_cast<unsigned char>(picture[1]) == 0xd8;
    return folder + "/covers/" + safe(id) + (jpeg ? ".jpg" : ".png");
}

/* The cover of a game that is on the console, either kind; "" without one. */
std::string kept_cover(const Shared &s, const std::string &source, const std::string &id)
{
    for (const char *extension : {".png", ".jpg"})
    {
        const std::string path = source_folder(s, source) + "/covers/" + safe(id) + extension;
        if (s.covers.count(path))
            return path;
    }
    return "";
}

std::shared_ptr<SourceState> find_source(const Shared &s, const std::string &key)
{
    for (const auto &state : s.sources)
        if (state->key == key)
            return state;
    return nullptr;
}

const Game *find_game(const Shared &s, const std::string &source, const std::string &id)
{
    if (const auto state = find_source(s, source))
        for (const Game &game : state->games)
            if (game.id == id)
                return &game;
    return nullptr;
}

/* A source's games as its kept list has them. */
Json catalog_games(const std::vector<Game> &list)
{
    Json games = Json::list();
    for (const Game &game : list)
    {
        Json item = Json::record();
        item.set("id", Json::of(game.id));
        item.set("name", Json::of(game.name));
        item.set("platform", Json::of(game.platform));
        item.set("folder", Json::of(game.folder));
        item.set("file", Json::of(game.file));
        item.set("cover", Json::of(game.cover_source));
        Json &parts = item.set("parts", Json::list());
        for (const Part &part : game.parts)
        {
            Json &entry = parts.push(Json::record());
            entry.set("id", Json::of(part.id));
            entry.set("name", Json::of(part.name));
            entry.set("size", Json::of(double(part.size)));
            entry.set("crc32", Json::of(part.crc32));
            entry.set("md5", Json::of(part.md5));
            entry.set("sha1", Json::of(part.sha1));
        }
        Json &ids = item.set("ids", Json::record());
        for (const auto &id : game.ids)
            ids.set(id.first, Json::of(id.second));
        item.set("identified", Json::of(game.identified));
        games.push(std::move(item));
    }
    return games;
}

void save_catalog(const Shared &s, const SourceState &state)
{
    Json catalog = Json::record();
    catalog.set("signature", Json::of(state.signature));
    catalog.set("games", catalog_games(state.games));
    const std::string folder = source_folder(s, state.key);
    if (!files::make_folders(folder) || !files::write(folder + "/catalog.json", catalog.write()))
        log("could not write the game list of " + state.name);
}

void load_catalog(Shared &s, SourceState &state)
{
    state.games.clear();
    std::string text_;
    Json catalog;
    if (!files::read(source_folder(s, state.key) + "/catalog.json", &text_) ||
        !Json::parse(text_, &catalog))
        return;
    /* A list from another entry (another server, other settings) is not this one's. */
    if (text(catalog, "signature") != state.signature)
        return;
    for (const Json &item : catalog["games"].items)
    {
        Game game;
        game.source = state.key;
        game.id = text(item, "id");
        game.name = text(item, "name");
        game.platform = text(item, "platform");
        game.folder = text(item, "folder");
        game.file = text(item, "file");
        game.cover_source = text(item, "cover");
        for (const Json &entry : item["parts"].items)
        {
            game.parts.push_back({text(entry, "id"), part_path(text(entry, "name")),
                                  entry["size"].whole(), text(entry, "crc32"), text(entry, "md5"),
                                  text(entry, "sha1")});
            game.size += game.parts.back().size;
            if (game.parts.back().name == game.file)
                game.crc32 = normal_crc(game.parts.back().crc32);
        }
        for (const auto &key : item["ids"].order)
            if (!text(item["ids"], key.c_str()).empty())
                game.ids[key] = text(item["ids"], key.c_str());
        game.identified = item["identified"].yes();
        game.normal_name = normal_name(game.name);
        const bool usable = !game.id.empty() && !game.platform.empty() && !game.parts.empty() &&
                            !game.folder.empty() &&
                            std::all_of(game.parts.begin(), game.parts.end(), [](const Part &part)
                                        { return !part.id.empty() && !part.name.empty(); });
        if (!usable)
            continue;
        for (const char *extension : {".png", ".jpg"})
        {
            const std::string cover =
                source_folder(s, state.key) + "/covers/" + safe(game.id) + extension;
            if (files::size(cover) > 0)
                s.covers.insert(cover);
        }
        state.games.push_back(std::move(game));
    }
}

void save_queue(Shared &s)
{
    Json entries = Json::list();
    for (const Entry &entry : s.queue)
    {
        Json &item = entries.push(Json::record());
        item.set("source", Json::of(entry.source));
        item.set("id", Json::of(entry.id));
    }
    if (!files::write(s.paths.config + "/queue.json", entries.write()))
        log("could not write queue.json");
}

void load_queue(Shared &s)
{
    std::string text_;
    Json entries;
    if (!files::read(s.paths.config + "/queue.json", &text_) || !Json::parse(text_, &entries))
        return;
    for (const Json &item : entries.items)
    {
        if (text(item, "source").empty() || text(item, "id").empty())
            continue;
        Entry entry;
        entry.serial = ++s.serials;
        entry.source = text(item, "source");
        entry.id = text(item, "id");
        s.queue.push_back(entry);
    }
}

/* Where each source's downloaded games are: config/remote/<source>/placed.json, the game's
 * folder by its id (a folder of the same name that was there already makes it another). */
Json &placed_of(Shared &s, const std::string &source)
{
    auto found = s.placed_games.find(source);
    if (found != s.placed_games.end())
        return found->second;
    std::string text_;
    Json placed;
    if (!files::read(source_folder(s, source) + "/placed.json", &text_) ||
        !Json::parse(text_, &placed) || placed.kind != Json::object)
        placed = Json::record();
    return s.placed_games[source] = std::move(placed);
}

void write_placed(Shared &s, const std::string &source)
{
    files::make_folders(source_folder(s, source));
    if (!files::write(source_folder(s, source) + "/placed.json", placed_of(s, source).write(true)))
        log("could not write placed.json of " + source);
}

/* The folder a game of a source is in on the console; "" when it is not there. */
std::string folder_of(Shared &s, const std::string &source, const std::string &id)
{
    const std::string folder = text(placed_of(s, source), id.c_str());
    return !folder.empty() && files::is_folder(folder) ? folder : std::string();
}

/* ---- sources.json ---- */

/* The sources of sources.json, keeping those whose entry did not change (with their lists). */
void read_sources(Shared &s)
{
    std::vector<std::shared_ptr<SourceState>> sources;
    s.configured = false;
    s.error.clear();
    std::string text_;
    if (files::read(s.paths.config + "/sources.json", &text_, 1u << 20))
    {
        Json json;
        if (!Json::parse(text_, &json) || json.kind != Json::object ||
            json["sources"].kind != Json::array)
            s.error = "sources.json is not valid: it needs a \"sources\" list";
        else
        {
            for (const Json &entry : json["sources"].items)
            {
                if (entry.kind != Json::object)
                    continue;
                const std::string type = lower(text(entry, "type"));
                std::string name = text(entry, "name");
                if (name.empty())
                    name = type.empty() ? "?" : type;
                /* Names tell the sources apart in the menu: a second "Home" is "Home (2)". */
                const std::string named = name;
                for (int n = 2; std::any_of(sources.begin(), sources.end(),
                                            [&](const auto &other) { return other->name == name; });
                     ++n)
                    name = named + " (" + std::to_string(n) + ")";
                /* Its key: from its name, told apart from another one of the same name. */
                std::string key = lower(safe(name));
                for (int n = 2; std::any_of(sources.begin(), sources.end(),
                                            [&](const auto &other) { return other->key == key; });
                     ++n)
                    key = lower(safe(name)) + "-" + std::to_string(n);
                auto state = std::make_shared<SourceState>();
                state->key = key;
                state->name = name;
                state->signature = std::to_string(std::hash<std::string>{}(entry.write()));
                if (const auto before = find_source(s, key);
                    before && before->signature == state->signature)
                {
                    sources.push_back(before);
                    continue;
                }
                std::string error;
                state->source = make_source(type, entry, &error);
                state->error = error;
                load_catalog(s, *state);
                if (!state->source)
                    log("source " + name + ": " + error);
                sources.push_back(std::move(state));
            }
            s.configured = !sources.empty();
        }
    }
    if (sources.size() != s.sources.size() ||
        !std::equal(sources.begin(), sources.end(), s.sources.begin()))
        ++s.generation;
    s.sources = std::move(sources);
}

/* ---- downloading ---- */

/* A game's files while it downloads. */
std::string staged_folder(const Paths &paths, const std::string &source, const std::string &id)
{
    return paths.downloads + "/" + safe(source) + "/" + safe(id);
}
std::string staged_path(const Paths &paths, const Game &game, const Part &part)
{
    return staged_folder(paths, game.source, game.id) + "/" + part.name;
}

/* A download's file written here (without a writer of the title's: the host's tests). */
class FileWriter final : public Writer
{
  public:
    ~FileWriter() override
    {
        if (file_)
            std::fclose(file_);
    }
    bool open(const std::string &path, uint64_t offset, std::string *error) override
    {
        if (file_)
            std::fclose(file_);
        file_ = std::fopen(path.c_str(), files::size(path) >= 0 ? "r+b" : "wb");
        if (!file_ || ftruncate(fileno(file_), static_cast<off_t>(offset)) != 0 ||
            std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0)
        {
            *error = "Cannot write " + path;
            return false;
        }
        return true;
    }
    bool write(const void *data, size_t size, std::string *error) override
    {
        if (std::fwrite(data, 1, size, file_) == size)
            return true;
        *error = "Cannot write the file (is the drive full?)";
        return false;
    }
    bool finish(std::string *error) override
    {
        const bool closed = file_ && std::fclose(file_) == 0;
        file_ = nullptr;
        if (!closed)
            *error = "Cannot write the file (is the drive full?)";
        return closed;
    }

  private:
    std::FILE *file_ = nullptr;
};

/* The bytes of a download, to its file in .remote-downloads/ through the writer. Its contents
 * are checked as they go by (StreamCheck). */
class FileReceiver final : public Receiver
{
  public:
    FileReceiver(Writer &writer, std::string path, uint64_t base, uint64_t start,
                 StreamCheck &check)
        : writer_(writer), path_(std::move(path)), base_(base), start_(start), check_(check)
    {
    }

    bool begin(bool from_start) override
    {
        /* The whole file, although a part was asked for: it is written again from its start. */
        if (from_start && start_ > 0)
        {
            start_ = 0;
            check_.restart();
            return writer_.open(path_, 0, &error_);
        }
        return true;
    }
    bool take(const void *data, size_t size) override
    {
        check_.feed(start_ + written_, data, size);
        if (!writer_.write(data, size, &error_))
            return false;
        written_ += size;
        Shared &s = state();
        s.current_done.store(base_ + start_ + written_);
        /* The speed: measured each second, smoothed so that it does not jump. */
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - sampled_).count();
        if (seconds >= 1.0)
        {
            const double measured = double(written_ - sampled_bytes_) / seconds;
            const double before = double(s.current_rate.load());
            s.current_rate.store(uint64_t(before > 0.0 ? before * 0.7 + measured * 0.3 : measured));
            sampled_ = now;
            sampled_bytes_ = written_;
        }
        return true;
    }
    bool stopped() override
    {
        const Shared &s = state();
        return s.halt.load() || (s.cancel.load() != 0 && s.cancel.load() == s.current.load());
    }
    /* Why the file was not written; empty when it was. */
    const std::string &error() const
    {
        return error_;
    }

  private:
    Writer &writer_;
    std::string path_;
    uint64_t base_;  /* the game's bytes before this file */
    uint64_t start_; /* where in the file the transfer began */
    StreamCheck &check_;
    uint64_t written_ = 0;
    std::string error_;
    Clock::time_point sampled_ = Clock::now(); /* when the speed was last measured */
    uint64_t sampled_bytes_ = 0;               /* written_ then */
};

enum class Outcome
{
    done,
    stopped,
    failed
};

/* The queue entry being downloaded shows the step it is in. */
void show_state(State step)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    for (Entry &entry : s.queue)
        if (entry.serial == s.current.load())
            entry.state = step;
}

/* A download that goes on from where it was: what its file has before `start`, read from the
 * drive for the check of its contents (the queue shows it as verifying meanwhile). False when
 * it was asked to stop; a file that cannot be read is left unchecked (the check sees the gap). */
bool feed_from_drive(StreamCheck &check, const std::string &path, uint64_t base, uint64_t start)
{
    Shared &s = state();
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return true;
    show_state(State::verifying);
    std::vector<char> buffer(1u << 20);
    bool stopped = false;
    for (uint64_t done = 0; done < start;)
    {
        if (s.halt.load() || (s.cancel.load() != 0 && s.cancel.load() == s.current.load()))
        {
            stopped = true;
            break;
        }
        const size_t want = size_t(std::min<uint64_t>(buffer.size(), start - done));
        const size_t got = std::fread(buffer.data(), 1, want, file);
        if (got == 0)
            break;
        check.feed(done, buffer.data(), got);
        done += got;
        s.current_done.store(base + done);
    }
    std::fclose(file);
    show_state(State::downloading);
    return !stopped;
}

std::string size_text(uint64_t bytes)
{
    char out[32];
    if (bytes >= (1ull << 30))
        std::snprintf(out, sizeof out, "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    else
        std::snprintf(out, sizeof out, "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return out;
}

Outcome download_part(Source &source, const Paths &paths, const Game &game, const Part &part,
                      uint64_t base, std::string *error)
{
    const std::string staged = staged_path(paths, game, part);
    if (!files::make_folders(files::parent(staged)))
    {
        *error = "Cannot write to " + files::parent(staged);
        return Outcome::failed;
    }
    /* A file of its full size is gone on with as well: after a power cut, its end may not hold
     * what was written there. */
    int64_t existing = files::size(staged);
    if (existing < 0 || (part.size > 0 && static_cast<uint64_t>(existing) > part.size))
    {
        (void)std::remove(staged.c_str());
        existing = -1;
    }
    const uint64_t start = existing > 0 && static_cast<uint64_t>(existing) > rewind_bytes
                               ? static_cast<uint64_t>(existing) - rewind_bytes
                               : 0;
    uint64_t space = 0;
    if (part.size > 0 && free_space(files::parent(staged), &space) && space < part.size - start)
    {
        *error = "Not enough free space: " + size_text(part.size - start) + " needed, " +
                 size_text(space) + " free";
        return Outcome::failed;
    }
    /* Going on: written over from `start` (the rewound bytes come again), else a new file. */
    StreamCheck check(part.crc32);
    if (start > 0 && !feed_from_drive(check, staged, base, start))
        return Outcome::stopped;
    const std::unique_ptr<Writer> writer =
        paths.writer ? paths.writer() : std::unique_ptr<Writer>(new FileWriter);
    if (!writer->open(staged, start, error))
        return Outcome::failed;
    state().current_done.store(base + start);
    FileReceiver receiver(*writer, staged, base, start, check);
    const bool fetched = source.fetch(
        as_source_game(game),
        {part.id, part.name, part.size, FileKind::game, part.crc32, part.md5, part.sha1}, start,
        receiver, error);
    std::string written;
    const bool finished = writer->finish(&written);
    /* Stopped or failed: what the file has stays, to go on from. */
    if (receiver.stopped())
        return Outcome::stopped;
    if (!receiver.error().empty() || !finished)
    {
        *error = !receiver.error().empty() ? receiver.error() : written;
        return Outcome::failed;
    }
    if (!fetched)
        return Outcome::failed;
    const int64_t have = files::size(staged);
    if (part.size > 0 && have != static_cast<int64_t>(part.size))
    {
        *error = "The download is incomplete (" +
                 size_text(static_cast<uint64_t>(std::max<int64_t>(have, 0))) + " of " +
                 size_text(part.size) + ")";
        return Outcome::failed;
    }
    /* A damaged file is deleted, so that trying again downloads it again. */
    switch (check.result())
    {
    case Verified::damaged:
        (void)std::remove(staged.c_str());
        *error =
            "The downloaded file " + part.name + " is damaged; trying again downloads it again";
        return Outcome::failed;
    case Verified::intact:
        log("download of " + game.name + ": " + part.name + " checked");
        break;
    default:
        log("download of " + game.name + ": " + part.name + " could not be checked");
    }
    return Outcome::done;
}

/* A game whose files are all downloaded: into its place at once, its folder as the source
 * names it, another name when a folder of that name is there already (the player's own). */
bool place_game(Shared &s, const Game &game, Placed *placed, std::string *error)
{
    const Paths &paths = s.paths;
    const std::string system = paths.system_folder ? paths.system_folder(game.platform)
                                                   : paths.content + "/" + game.platform;
    if (!files::make_folders(system))
    {
        *error = "Cannot write to " + system;
        return false;
    }
    std::string folder = system + "/" + game.folder;
    for (int n = 2; files::is_folder(folder) || files::size(folder) >= 0; n++)
        folder = system + "/" + game.folder + " (" + std::to_string(n) + ")";
    const std::string staged = staged_folder(paths, game.source, game.id);
    if (std::rename(staged.c_str(), folder.c_str()) != 0)
    {
        *error = "Cannot move the download into " + system;
        return false;
    }
    (void)rmdir((paths.downloads + "/" + safe(game.source)).c_str());
    (void)rmdir(paths.downloads.c_str());
    placed_of(s, game.source).set(game.id, Json::of(folder));
    write_placed(s, game.source);
    placed->game = game;
    placed->folder = folder;
    placed->launch = folder + "/" + game.file;
    return true;
}

/* What of a game is on the console so far: what its download has. */
uint64_t present_of(const Paths &paths, const Game &game, const Part &part)
{
    return static_cast<uint64_t>(std::max<int64_t>(files::size(staged_path(paths, game, part)), 0));
}

uint64_t present(const Paths &paths, const Game &game)
{
    uint64_t bytes = 0;
    for (const Part &part : game.parts)
        bytes += present_of(paths, game, part);
    return bytes;
}

/* Unfinished downloads of games that are no longer in the queue (cancelled while one was
 * still writing, a queue file that was lost, a source taken out of sources.json): nothing of
 * them is kept. */
void remove_leftovers(const Shared &s)
{
    for (const std::string &source : files::names(s.paths.downloads))
    {
        const std::string folder = s.paths.downloads + "/" + source;
        for (const std::string &game : files::names(folder))
            if (std::none_of(s.queue.begin(), s.queue.end(), [&](const Entry &e)
                             { return safe(e.source) == source && safe(e.id) == game; }))
                files::remove_tree(folder + "/" + game);
        (void)rmdir(folder.c_str());
    }
    (void)rmdir(s.paths.downloads.c_str());
}

/* ---- the threads ---- */

void *list_thread(void *)
{
    Shared &s = state();
    std::unique_lock<std::mutex> lock(s.lock);
    for (;;)
    {
        const auto wanted = [&]
        {
            for (const auto &state : s.sources)
                if (state->refresh_wanted && state->source)
                    return state;
            return std::shared_ptr<SourceState>{};
        };
        s.wake.wait(lock, [&] { return !s.halt.load() && wanted() != nullptr; });
        const std::shared_ptr<SourceState> state = wanted();
        state->refresh_wanted = false;
        state->refreshing = true;
        s.listing = true;
        const std::shared_ptr<Source> source = state->source;
        const std::string key = state->key;
        lock.unlock();
        std::vector<SourceGame> listed;
        std::string error;
        const bool read = source->list(&listed, &error, [&s] { return s.halt.load(); });
        std::vector<Game> games;
        for (const SourceGame &from : listed)
        {
            Game game;
            if (make_game(key, from, &game))
                games.push_back(std::move(game));
        }
        lock.lock();
        state->refreshing = false;
        /* A source taken out of sources.json meanwhile is not this one any more. */
        if (!read || find_source(s, key) != state)
        {
            s.listing = false;
            if (find_source(s, key) == state && !s.halt.load())
            {
                state->online = false;
                state->error = error;
                log(state->name + ": " + error);
            }
            else if (s.halt.load())
                state->refresh_wanted = true; /* read when the menu is back */
            s.wake.notify_all();
            continue;
        }
        /* A cover that changed on the source is fetched again. */
        for (const Game &game : games)
            for (const Game &before : state->games)
                if (before.id == game.id && before.cover_source != game.cover_source)
                    for (const char *extension : {".png", ".jpg"})
                    {
                        const std::string cover =
                            source_folder(s, key) + "/covers/" + safe(game.id) + extension;
                        (void)std::remove(cover.c_str());
                        s.covers.erase(cover);
                    }
        /* A new generation only for a list that is not the one it has. */
        const bool changed = catalog_games(games) != catalog_games(state->games);
        state->games = std::move(games);
        state->online = true;
        state->error.clear();
        state->listed_once = true;
        state->listed_at = Clock::now();
        if (changed)
        {
            ++s.generation;
            save_catalog(s, *state);
        }
        s.wake.notify_all(); /* queued games that waited for the list */
        std::vector<Game> covers;
        for (const Game &game : state->games)
            if (!game.cover_source.empty() && kept_cover(s, key, game.id).empty())
                covers.push_back(game);
        /* Where they go, while the lock is held (start sets the paths again). */
        const std::string folder = source_folder(s, key);
        lock.unlock();
        std::vector<std::string> written;
        for (const Game &game : covers)
        {
            if (s.halt.load())
                break;
            std::string picture;
            if (!source->cover(as_source_game(game), &picture, [&s] { return s.halt.load(); }))
                continue;
            const std::string path = cover_path(folder, game.id, picture);
            if (files::make_folders(files::parent(path)) && files::write(path, picture))
                written.push_back(path);
            else
                log("the cover of " + game.name + " cannot be written");
        }
        lock.lock();
        s.covers.insert(written.begin(), written.end());
        if (!written.empty())
            ++s.generation;
        s.listing = false;
        s.wake.notify_all();
    }
    return nullptr;
}

void *download_thread(void *)
{
    Shared &s = state();
    std::unique_lock<std::mutex> lock(s.lock);
    for (;;)
    {
        /* A game the kept list does not have (a new entry in sources.json) waits for the
         * source's list. */
        const auto next = [&]
        {
            return std::find_if(s.queue.begin(), s.queue.end(),
                                [&](const Entry &entry)
                                {
                                    if (entry.state != State::queued)
                                        return false;
                                    const auto state = find_source(s, entry.source);
                                    return state == nullptr || state->source == nullptr ||
                                           state->listed_once ||
                                           find_game(s, entry.source, entry.id) != nullptr;
                                });
        };
        /* A cancelled game's files go also while a game runs (a cancel says they are gone). */
        s.wake.wait(lock, [&]
                    { return !s.discards.empty() || (!s.halt.load() && next() != s.queue.end()); });
        if (!s.discards.empty())
        {
            const std::vector<Discard> discards = std::move(s.discards);
            s.discards.clear();
            const Paths paths = s.paths;
            lock.unlock();
            for (const Discard &discard : discards)
            {
                files::remove_tree(staged_folder(paths, discard.source, discard.id));
                (void)rmdir((paths.downloads + "/" + safe(discard.source)).c_str());
                (void)rmdir(paths.downloads.c_str());
            }
            lock.lock();
            continue;
        }
        Entry &entry = *next();
        const auto source_state = find_source(s, entry.source);
        const Game *found = find_game(s, entry.source, entry.id);
        if (source_state == nullptr || source_state->source == nullptr || found == nullptr)
        {
            entry.state = State::failed;
            entry.error = source_state == nullptr ? "Its source is no longer in sources.json"
                          : source_state->source == nullptr ? source_state->error
                                                            : "Its source no longer has this game";
            continue;
        }
        const uint64_t serial = entry.serial;
        Game game = *found;
        game.cover = kept_cover(s, game.source, game.id);
        const std::shared_ptr<Source> source = source_state->source;
        const Paths paths = s.paths;
        entry.state = State::downloading;
        entry.error.clear();
        entry.total = game.size;
        s.current.store(serial);
        s.current_done.store(present(paths, game));
        s.current_rate.store(0);
        s.transferring = true;
        lock.unlock();

        /* Each file to .remote-downloads/, then all of them to their place (place_game). */
        Outcome outcome = Outcome::done;
        std::string error;
        for (const Part &part : game.parts)
        {
            /* What the game's other files have on the console, for the progress. */
            const uint64_t base = present(paths, game) - present_of(paths, game, part);
            outcome = download_part(*source, paths, game, part, base, &error);
            if (outcome != Outcome::done)
                break;
        }

        lock.lock();
        Placed placed;
        if (outcome == Outcome::done && s.cancel.load() != serial &&
            !place_game(s, game, &placed, &error))
            outcome = Outcome::failed;
        s.transferring = false;
        s.current.store(0);
        const bool cancelled = s.cancel.load() == serial;
        if (cancelled)
            s.cancel.store(0);
        const auto at = std::find_if(s.queue.begin(), s.queue.end(),
                                     [&](const Entry &e) { return e.serial == serial; });
        if (cancelled)
        {
            s.discards.push_back({game.source, game.id});
            if (at != s.queue.end())
                s.queue.erase(at);
            save_queue(s);
        }
        else if (outcome == Outcome::done)
        {
            if (at != s.queue.end())
                s.queue.erase(at);
            save_queue(s);
            ++s.generation;
            const PlacedNote note = s.placed;
            lock.unlock();
            if (note)
                note(placed);
            lock.lock();
        }
        else if (at != s.queue.end())
        {
            at->done = present(paths, game);
            if (outcome == Outcome::stopped)
                at->state = State::queued; /* goes on when the menu is back */
            else
            {
                at->state = State::failed;
                at->error = error;
                log("download of " + game.name + " failed: " + error);
            }
        }
        s.wake.notify_all();
    }
    return nullptr;
}

/* sources.json and the kept lists; the queue once. */
void read_locked(Shared &s, const Paths &paths)
{
    s.paths = paths;
    files::make_folders(paths.config);
    read_sources(s);
    if (!s.queue_read)
    {
        s.queue_read = true;
        load_queue(s);
        /* What each one has on the console already, before its download goes on. */
        for (Entry &entry : s.queue)
            if (const Game *game = find_game(s, entry.source, entry.id))
            {
                entry.total = game->size;
                entry.done = present(s.paths, *game);
            }
    }
}
} // namespace

/* ---- which game is which ---- */

namespace
{
/* Latin-1's letters (U+00C0-U+00FF) and Latin Extended-A (U+0100-U+017F) as plain letters;
 * '*' is a letter of two (Æ, Œ, ĳ...), '-' no letter (×, ÷). */
const char latin1[] = "AAAAAA*CEEEEIIIIDNOOOOO-OUUUUY*saaaaaa*ceeeeiiiidnooooo-ouuuuy*y";
const char extended[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi**JjKkkLlLlLlLlLlNn"
                        "NnNnnNnOoOoOo**RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static_assert(sizeof(latin1) == 0x40 + 1, "one letter for each of U+00C0-U+00FF");
static_assert(sizeof(extended) == 0x80 + 1, "one letter for each of U+0100-U+017F");
const std::map<uint32_t, const char *> pairs = {{0xC6, "AE"},  {0xDE, "TH"},  {0xDF, "ss"},
                                                {0xE6, "ae"},  {0xFE, "th"},  {0x132, "IJ"},
                                                {0x133, "ij"}, {0x152, "OE"}, {0x153, "oe"}};

/* Each character of a UTF-8 text: its code point, its bytes, and its Latin letters without
 * their accents ("" when it is no Latin letter). */
void each_character(const std::string &text,
                    const std::function<void(uint32_t code, const std::string &bytes,
                                             const std::string &plain)> &take)
{
    for (size_t at = 0; at < text.size();)
    {
        const unsigned char first = static_cast<unsigned char>(text[at]);
        uint32_t code = first;
        size_t length = 1;
        if (first >= 0xF0)
            code = first & 0x07, length = 4;
        else if (first >= 0xE0)
            code = first & 0x0F, length = 3;
        else if (first >= 0xC0)
            code = first & 0x1F, length = 2;
        if (at + length > text.size())
            break;
        for (size_t i = 1; i < length; ++i)
            code = (code << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
        const std::string bytes = text.substr(at, length);
        at += length;
        std::string plain;
        if (const auto pair = pairs.find(code); pair != pairs.end())
            plain = pair->second;
        else if (code >= 0xC0 && code <= 0xFF && latin1[code - 0xC0] != '-')
            plain = std::string(1, latin1[code - 0xC0]);
        else if (code >= 0x100 && code <= 0x17F)
            plain = std::string(1, extended[code - 0x100]);
        take(code, bytes, plain);
    }
}
} // namespace

std::string normal_name(const std::string &name)
{
    std::string out;
    int depth = 0; /* inside a tag: "(USA)", "[!]" */
    each_character(name,
                   [&](uint32_t code, const std::string &bytes, const std::string &plain)
                   {
                       if (code == '(' || code == '[')
                           depth++;
                       else if ((code == ')' || code == ']') && depth > 0)
                           depth--;
                       else if (depth > 0)
                           return;
                       else if (code < 0x80)
                       {
                           if ((code >= '0' && code <= '9') || (code >= 'a' && code <= 'z'))
                               out += static_cast<char>(code);
                           else if (code >= 'A' && code <= 'Z')
                               out += static_cast<char>(code - 'A' + 'a');
                       }
                       else if (!plain.empty())
                           out += lower(plain == "*" ? "" : plain);
                       else if (code >= 0x370 && !(code >= 0x2000 && code <= 0x2BFF) &&
                                !(code >= 0x3000 && code <= 0x303F) &&
                                !(code >= 0xFE00 && code <= 0xFE6F) &&
                                !(code >= 0xFF00 && code <= 0xFF0F))
                           /* Other scripts (Greek, Cyrillic, Japanese...) stay as they are;
                            * punctuation, symbols (™ ®), marks and spaces go. */
                           out += bytes;
                   });
    return out;
}

std::string plain_text(const std::string &text)
{
    std::string out;
    each_character(text,
                   [&](uint32_t code, const std::string &, const std::string &plain)
                   {
                       if (code >= 0x20 && code < 0x7f)
                           out += static_cast<char>(code);
                       else if (!plain.empty() && plain != "*")
                           out += plain;
                       else if (code >= 0x80)
                           out += '?';
                   });
    return out;
}

std::string normal_crc(const std::string &crc)
{
    std::string digits = crc.substr(0, crc.find('|'));
    if (digits.size() == 10 && (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0))
        digits = digits.substr(2);
    if (digits.size() != 8 ||
        digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return "";
    digits = lower(digits);
    return digits == "00000000" ? "" : digits;
}

bool same_game(const Game &a, const Game &b)
{
    if (a.platform != b.platform)
        return false;
    if (!a.crc32.empty() && !b.crc32.empty())
        return a.crc32 == b.crc32;
    /* An id at a metadata provider both have one of: the first one decides. */
    for (const auto &id : a.ids)
        if (const auto other = b.ids.find(id.first); other != b.ids.end())
            return id.second == other->second;
    if (a.identified && b.identified)
    {
        const std::string name = a.normal_name.empty() ? normal_name(a.name) : a.normal_name;
        return !name.empty() &&
               name == (b.normal_name.empty() ? normal_name(b.name) : b.normal_name);
    }
    return lower(files::base_name(a.file)) == lower(files::base_name(b.file));
}

bool as_game(const std::string &source, const SourceGame &from, Game *game)
{
    return make_game(source, from, game);
}

bool same_as_local(const Game &game, const std::string &crc32, const std::string &normal,
                   const std::string &file)
{
    /* The file it is downloaded as: whatever the checksums say (a server's can be of another
     * dump than the one on the console), it is this game, and not offered again. */
    if (lower(files::base_name(game.file)) == lower(files::base_name(file)))
        return true;
    const std::string crc = normal_crc(crc32);
    if (!game.crc32.empty() && !crc.empty())
        return game.crc32 == crc;
    return game.identified && !game.normal_name.empty() && game.normal_name == normal;
}

std::string title_key(const Game &game)
{
    if (!game.crc32.empty())
        return "crc:" + game.crc32;
    if (!game.ids.empty())
        return game.ids.begin()->first + ":" + game.ids.begin()->second;
    if (game.identified && !normal_name(game.name).empty())
        return "name:" + normal_name(game.name);
    return "file:" + lower(game.file);
}

std::string launch_file(const std::vector<std::string> &paths, const std::vector<uint64_t> &sizes)
{
    static const char *const preferred[] = {"m3u", "cue", "gdi", "ccd", "chd"};
    const auto extension = [](const std::string &path)
    {
        const std::string name = files::base_name(path);
        const size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
    };
    for (const char *wanted : preferred)
    {
        std::vector<std::string> found;
        for (const auto &path : paths)
            if (extension(path) == wanted)
                found.push_back(path);
        if (!found.empty())
            return *std::min_element(found.begin(), found.end());
    }
    size_t largest = 0;
    for (size_t i = 1; i < paths.size() && i < sizes.size(); i++)
        if (sizes[i] > sizes[largest])
            largest = i;
    return paths.empty() ? "" : paths[largest];
}

/* ---- the lists and the queue ---- */

void read(const Paths &paths)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    read_locked(s, paths);
}

void list_new(const Paths &paths, unsigned timeout)
{
    Shared &s = state();
    std::vector<std::shared_ptr<SourceState>> fresh;
    {
        std::lock_guard<std::mutex> lock(s.lock);
        read_locked(s, paths);
        for (const auto &state : s.sources)
            if (state->source && !state->listed_once &&
                files::size(source_folder(s, state->key) + "/catalog.json") < 0)
                fresh.push_back(state);
    }
    for (const auto &state : fresh)
    {
        std::vector<SourceGame> listed;
        std::string error;
        const bool read_ = state->source->list(&listed, &error, [] { return false; }, timeout);
        std::lock_guard<std::mutex> lock(s.lock);
        /* Not listed_once: the list thread reads it again once RetroArch is up, with the
         * covers. */
        state->online = read_;
        state->error = read_ ? "" : error;
        state->listed_at = Clock::now();
        state->games.clear();
        if (!read_)
        {
            /* Its (empty) list is kept: the next start does not wait for it again, the list
             * thread tries it once RetroArch is up. */
            log(state->name + ": " + error);
            save_catalog(s, *state);
            continue;
        }
        for (const SourceGame &from : listed)
        {
            Game game;
            if (make_game(state->key, from, &game))
                state->games.push_back(std::move(game));
        }
        save_catalog(s, *state);
        ++s.generation;
    }
}

void start(const Paths &paths, PlacedNote placed)
{
    Shared &s = state();
    std::unique_lock<std::mutex> lock(s.lock);
    read_locked(s, paths);
    s.placed = std::move(placed);
    remove_leftovers(s);
    for (Entry &entry : s.queue)
        /* One whose download did not end before the menu was away; not the one still in a
         * transfer (stop does not wait for ever). */
        if ((entry.state == State::downloading || entry.state == State::verifying) &&
            !(s.transferring && entry.serial == s.current.load()))
            entry.state = State::queued;
    for (const auto &state : s.sources)
        if (state->source && (!state->listed_once || Clock::now() - state->listed_at > refresh_age))
            state->refresh_wanted = true;
    s.halt.store(false);
    if (!s.threads)
    {
        s.threads = true;
        pthread_t thread;
        if (create_title_thread(&thread, list_thread, nullptr) == 0)
            pthread_detach(thread);
        if (create_title_thread(&thread, download_thread, nullptr) == 0)
            pthread_detach(thread);
    }
    s.wake.notify_all();
}

void stop()
{
    Shared &s = state();
    std::unique_lock<std::mutex> lock(s.lock);
    s.halt.store(true);
    s.wake.notify_all();
    /* A transfer asks whether to stop at least once a second. */
    (void)s.wake.wait_for(lock, std::chrono::seconds(3),
                          [&] { return !s.transferring && !s.listing; });
}

void refresh()
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    for (const auto &state : s.sources)
        if (state->source)
            state->refresh_wanted = true;
    s.wake.notify_all();
}

void changed()
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    ++s.generation;
}

Status current()
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    Status status;
    status.configured = s.configured;
    status.error = s.error;
    status.generation = s.generation;
    for (const auto &state : s.sources)
    {
        SourceStatus source;
        source.key = state->key;
        source.name = state->name;
        source.address = state->source ? state->source->address() : std::string();
        source.refreshing = state->refreshing || state->refresh_wanted;
        source.online = state->online;
        source.error = state->error;
        source.games = state->games.size();
        status.sources.push_back(std::move(source));
    }
    return status;
}

std::vector<Game> games()
{
    Shared &s = state();
    std::vector<Game> list;
    std::lock_guard<std::mutex> lock(s.lock);
    for (const auto &state : s.sources)
        list.insert(list.end(), state->games.begin(), state->games.end());
    for (Game &game : list)
        game.cover = kept_cover(s, game.source, game.id);
    return list;
}

std::vector<Title> titles()
{
    std::vector<Title> list;
    for (Game &game : games())
    {
        /* With the title it is the same game as; a source's own games are never one title. */
        Title *title = nullptr;
        for (Title &other : list)
            if (std::none_of(other.games.begin(), other.games.end(),
                             [&](const Game &g) { return g.source == game.source; }) &&
                same_game(other.games.front(), game))
            {
                title = &other;
                break;
            }
        if (title == nullptr)
        {
            /* Its key, told apart from another title's (two copies of a game on one source). */
            std::string key = title_key(game);
            for (int n = 2; std::any_of(list.begin(), list.end(),
                                        [&](const Title &t) { return t.key == key; });
                 ++n)
                key = title_key(game) + "#" + std::to_string(n);
            list.push_back({key, {}});
            title = &list.back();
        }
        title->games.push_back(std::move(game));
    }
    return list;
}

bool find(const std::string &source, const std::string &id, Game *game)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    const Game *found = find_game(s, source, id);
    if (found == nullptr)
        return false;
    if (game)
    {
        *game = *found;
        game->cover = kept_cover(s, source, id);
    }
    return true;
}

std::vector<Download> downloads()
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    std::vector<Download> list;
    for (const Entry &entry : s.queue)
    {
        Download download;
        download.source = entry.source;
        download.id = entry.id;
        download.state = entry.state;
        const bool running = entry.state == State::downloading || entry.state == State::verifying;
        download.done = running ? s.current_done.load() : entry.done;
        download.rate = entry.state == State::downloading ? s.current_rate.load() : 0;
        download.total = entry.total;
        if (download.total == 0)
            if (const Game *game = find_game(s, entry.source, entry.id))
                download.total = game->size;
        download.error = entry.error;
        list.push_back(std::move(download));
    }
    return list;
}

bool enqueue(const std::string &source, const std::string &id, bool first)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    const Game *game = find_game(s, source, id);
    if (game == nullptr)
        return false;
    auto at = std::find_if(s.queue.begin(), s.queue.end(), [&](const Entry &entry)
                           { return entry.source == source && entry.id == id; });
    if (at == s.queue.end())
    {
        /* The same game from another source is already coming. */
        for (const Entry &entry : s.queue)
            if (const Game *other = find_game(s, entry.source, entry.id);
                other && same_game(*other, *game))
                return false;
        Entry entry;
        entry.serial = ++s.serials;
        entry.source = source;
        entry.id = id;
        entry.total = game->size;
        entry.done = present(s.paths, *game);
        if (first)
            s.queue.push_front(entry);
        else
            s.queue.push_back(entry);
    }
    else
    {
        if (at->state == State::failed)
        {
            at->state = State::queued;
            at->error.clear();
        }
        /* Cancelled while it downloads, and wanted again before the download stopped: it
         * goes on. */
        if (s.cancel.load() == at->serial)
            s.cancel.store(0);
        /* Played now: it comes before the others (the one downloading goes on first). */
        if (first && at->state == State::queued && at != s.queue.begin())
        {
            const Entry entry = *at;
            s.queue.erase(at);
            const auto after = std::find_if(
                s.queue.begin(), s.queue.end(), [](const Entry &e)
                { return e.state != State::downloading && e.state != State::verifying; });
            s.queue.insert(after, entry);
        }
    }
    save_queue(s);
    s.wake.notify_all();
    return true;
}

bool cancel(const std::string &source, const std::string &id)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    const auto at = std::find_if(s.queue.begin(), s.queue.end(), [&](const Entry &entry)
                                 { return entry.source == source && entry.id == id; });
    if (at == s.queue.end())
        return false;
    if (at->state == State::downloading || at->state == State::verifying)
    {
        s.cancel.store(at->serial); /* the download thread removes it and its files */
        return true;
    }
    s.discards.push_back({source, id});
    s.queue.erase(at);
    save_queue(s);
    s.wake.notify_all();
    return true;
}

std::string placed_folder(const std::string &source, const std::string &id)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    return folder_of(s, source, id);
}

void forget(const std::string &source, const std::string &id)
{
    Shared &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    placed_of(s, source).erase(id);
    write_placed(s, source);
    ++s.generation;
}
} // namespace ps5::remote
