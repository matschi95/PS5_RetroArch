/* PS5 RetroArch - when the save sync runs (src/remote/save_jobs.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "save_jobs.h"

#include "files.h"
#include "save_config.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace ps5::remote
{
namespace
{
std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

std::string outcome_text(const SyncResult &result)
{
    switch (result.outcome)
    {
    case SyncOutcome::uploaded:
        return "uploaded";
    case SyncOutcome::downloaded:
        return "downloaded";
    case SyncOutcome::kept:
        return "conflict left as it is";
    case SyncOutcome::no_game:
        return "the server does not have the game";
    case SyncOutcome::failed:
        return result.message;
    case SyncOutcome::same:
        break;
    }
    return "already in step";
}
} // namespace

SaveJobs::SaveJobs(std::string folder) : folder_(std::move(folder))
{
}

SaveJobs::~SaveJobs()
{
    stop();
    wait();
}

bool SaveJobs::wanted() const
{
    const SaveConfig config = read_save_config(folder_ + "/save-sync.json");
    return config.readable && !config.type.empty() && config.automatic;
}

void SaveJobs::on_notice(std::function<void(const std::string &)> notice)
{
    std::lock_guard<std::mutex> guard(lock_);
    notice_ = std::move(notice);
}

void SaveJobs::notice(const std::string &text_)
{
    if (text_.empty())
        return; /* nothing worth a notice */
    std::function<void(const std::string &)> notice;
    {
        std::lock_guard<std::mutex> guard(lock_);
        notice = notice_;
    }
    std::fprintf(stderr, "[save sync] %s\n", text_.c_str());
    if (notice)
        notice(text_);
}

SyncPlaces SaveJobs::places(bool states) const
{
    SyncPlaces places;
    places.store = folder_ + "/save-sync";
    places.work = places.store + "/work";
    places.backups = places.store + "/backups";
    places.states = states;
    return places;
}

SyncResult SaveJobs::sync_before(const SyncGame &game, unsigned seconds)
{
    SyncResult result;
    result.outcome = SyncOutcome::same;
    const SaveConfig config = read_save_config(folder_ + "/save-sync.json");
    if (!config.readable || config.type.empty() || !config.automatic)
        return result;
    /* The remote core synced it just now, asking. */
    const std::string marker = folder_ + "/save-sync/launched";
    std::string launched;
    if (files::read(marker, &launched))
    {
        std::remove(marker.c_str());
        if (launched == game.content)
            return result;
    }
    remember(game);
    /* What waits from after a game first: this one's save data may be among it. */
    wait();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    result = sync_save_data(
        config.type, config.settings, game, places(config.states),
        [](const SavePlan &, const LocalSave &) { return SyncChoice::neither; },
        [until] { return std::chrono::steady_clock::now() > until; });
    notice(sync_notice(game.name, result, true));
    return result;
}

void SaveJobs::synced_launch(const std::string &content)
{
    files::make_folders(folder_ + "/save-sync");
    files::write(folder_ + "/save-sync/launched", content);
}

bool SaveJobs::start(std::vector<SyncGame> games, bool before)
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (view_.stage == SyncView::working || view_.stage == SyncView::conflict)
            return false;
    }
    wait();
    const SaveConfig config = read_save_config(folder_ + "/save-sync.json");
    if (games.empty())
        return true;
    {
        std::lock_guard<std::mutex> guard(lock_);
        view_ = SyncView();
        view_.stage = SyncView::working;
        view_.before = before;
        view_.game = games.front().name;
        choice_.reset();
    }
    stop_ = false;
    thread_ = std::thread(
        [this, games = std::move(games), before, config]
        {
            SyncView last;
            last.stage = SyncView::done;
            last.before = before;
            for (const SyncGame &game : games)
            {
                if (stop_)
                    break;
                {
                    std::lock_guard<std::mutex> guard(lock_);
                    view_.game = game.name;
                    view_.stage = SyncView::working;
                }
                SyncResult result;
                /* Turned off ("auto"): neither before a game nor what waits from after one. */
                if (!config.readable || config.type.empty() || !config.automatic)
                    result.outcome = SyncOutcome::same;
                else
                {
                    result = sync_save_data(
                        config.type, config.settings, game, places(config.states),
                        [this, before](const SavePlan &plan, const LocalSave &local)
                        {
                            /* After a game nobody is asked (RetroArch's menu has no dialog for
                             * it): left as it is, and said. */
                            if (!before)
                                return SyncChoice::neither;
                            std::unique_lock<std::mutex> guard(lock_);
                            view_.stage = SyncView::conflict;
                            view_.file = local.name;
                            view_.console_time = local.updated;
                            view_.server_time = plan.updated;
                            view_.server_device = plan.device;
                            choice_.reset();
                            changed_.wait(guard, [this] { return choice_.has_value() || stop_; });
                            view_.stage = SyncView::working;
                            return choice_.value_or(SyncChoice::neither);
                        },
                        [this] { return stop_.load(); });
                    const std::string url = text(config.settings, "url");
                    last.server = result.user.empty() ? url : result.user + " @ " + url;
                    notice(sync_notice(game.name, result, before));
                }
                /* After a game: done with it unless it failed (then it is tried again at the
                 * next start), or it was stopped (a conflict not answered is no answer). */
                if (!before && result.outcome != SyncOutcome::failed && !stop_)
                {
                    std::lock_guard<std::mutex> guard(pending_lock_);
                    std::vector<SyncGame> waiting = pending();
                    for (size_t i = 0; i < waiting.size(); i++)
                        if (waiting[i].content == game.content)
                            waiting.erase(waiting.begin() + long(i--));
                    write_pending(waiting);
                }
                last.game = game.name;
                if (result.outcome == SyncOutcome::failed)
                {
                    last.stage = SyncView::failed;
                    last.error = result.message;
                    last.too_old = result.too_old;
                    last.server_version = result.version;
                    last.needed_version = result.needed;
                }
                else if (last.stage != SyncView::failed)
                    last.outcome = result.outcome;
            }
            std::lock_guard<std::mutex> guard(lock_);
            view_ = last;
        });
    return true;
}

void SaveJobs::played(const SyncGame &game)
{
    if (!wanted())
        return;
    {
        std::lock_guard<std::mutex> guard(pending_lock_);
        std::vector<SyncGame> waiting = pending();
        bool there = false;
        for (const SyncGame &other : waiting)
            there = there || other.content == game.content;
        if (!there)
            waiting.push_back(game);
        write_pending(waiting);
    }
    (void)start({game}, false); /* else at the next start */
}

void SaveJobs::resume_pending()
{
    if (!wanted())
        return;
    std::vector<SyncGame> waiting = pending();
    if (!waiting.empty())
        (void)start(std::move(waiting), false);
}

SyncView SaveJobs::view()
{
    std::lock_guard<std::mutex> guard(lock_);
    return view_;
}

void SaveJobs::choose(SyncChoice choice)
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        choice_ = choice;
    }
    changed_.notify_all();
}

void SaveJobs::stop()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    changed_.notify_all();
}

void SaveJobs::end()
{
    std::lock_guard<std::mutex> guard(lock_);
    if (view_.stage == SyncView::done || view_.stage == SyncView::failed)
        view_ = SyncView();
}

void SaveJobs::wait()
{
    if (thread_.joinable())
        thread_.join();
}

/* The games as the hook had them: <folder>/save-sync/games.json, by their content. */
void SaveJobs::remember(const SyncGame &game) const
{
    std::lock_guard<std::mutex> guard(pending_lock_);
    const std::string file = folder_ + "/save-sync/games.json";
    std::string content;
    Json games;
    if (!files::read(file, &content) || !Json::parse(content, &games) || games.kind != Json::object)
        games = Json::record();
    const Json entry = to_json(game);
    if (games[game.content] == entry)
        return;
    games.set(game.content, entry);
    files::make_folders(folder_ + "/save-sync");
    (void)files::write(file, games.write(true));
}

bool SaveJobs::known(const std::string &content_, SyncGame *game) const
{
    std::lock_guard<std::mutex> guard(pending_lock_);
    std::string content;
    Json games;
    if (!files::read(folder_ + "/save-sync/games.json", &content) ||
        !Json::parse(content, &games) || games[content_].kind != Json::object)
        return false;
    *game = from_json(games[content_]);
    return !game->save.empty();
}

std::vector<SyncGame> SaveJobs::pending() const
{
    std::vector<SyncGame> games;
    std::string content;
    Json list;
    if (!files::read(folder_ + "/save-sync/pending.json", &content) ||
        !Json::parse(content, &list) || list.kind != Json::array)
        return games;
    for (const Json &item : list.items)
        if (!text(item, "content").empty() && !text(item, "save").empty())
            games.push_back(from_json(item));
    return games;
}

void SaveJobs::write_pending(const std::vector<SyncGame> &games) const
{
    const std::string file = folder_ + "/save-sync/pending.json";
    if (games.empty())
    {
        std::remove(file.c_str());
        return;
    }
    Json list = Json::list();
    for (const SyncGame &game : games)
        list.push(to_json(game));
    files::make_folders(folder_ + "/save-sync");
    (void)files::write(file, list.write(true));
}

Json SaveJobs::to_json(const SyncGame &game)
{
    Json json = Json::record();
    json.set("platform", Json::of(game.platform));
    json.set("name", Json::of(game.name));
    json.set("content", Json::of(game.content));
    json.set("crc32", Json::of(game.crc32));
    json.set("emulator", Json::of(game.emulator));
    json.set("save", Json::of(game.save));
    json.set("state", Json::of(game.state));
    return json;
}

SyncGame SaveJobs::from_json(const Json &json)
{
    SyncGame game;
    game.platform = text(json, "platform");
    game.name = text(json, "name");
    game.content = text(json, "content");
    game.crc32 = text(json, "crc32");
    game.emulator = text(json, "emulator");
    game.save = text(json, "save");
    game.state = text(json, "state");
    return game;
}

std::string sync_notice(const std::string &game, const SyncResult &result, bool before)
{
    switch (result.outcome)
    {
    case SyncOutcome::uploaded:
        return game + ": save data uploaded";
    case SyncOutcome::downloaded:
        return game + ": save data from the server";
    case SyncOutcome::kept:
        return game + ": save data changed here and on the server; left as it is (Remote > "
                      "Save sync)";
    case SyncOutcome::failed:
        if (result.too_old)
            return "Save sync: RomM " + result.version + " is too old (needs " + result.needed +
                   ")";
        return game + ": save data not synced (" + outcome_text(result) + ")" +
               (before ? "; it stays as it is" : "; tried again later");
    case SyncOutcome::no_game:
    case SyncOutcome::same:
        break;
    }
    return "";
}
} // namespace ps5::remote
