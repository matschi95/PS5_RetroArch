/* PS5 RetroArch - the RomM backend of the save stores (src/remote/romm/romm_saves.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_saves.h"

#include "../files.h"
#include "romm_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <utility>

namespace ps5::remote::romm
{
namespace
{
/* Where a game's save is on the server, as RomM keeps RetroArch's. */
constexpr const char *slot = "autosave";
/* The versions of the slot the server keeps. */
constexpr int kept = 10;

std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

int64_t number(const Json &object, const char *key)
{
    return object[key].kind == Json::number ? int64_t(object[key].value) : 0;
}

std::string escape(const std::string &value)
{
    return ps5_scraper::url_encode(value);
}

/* What the server says went wrong: a refusal by the scope it needs (path: what was asked
 * for), else its "detail", else the status. */
std::string detail(const Client &client, const Client::Answer &answer, const std::string &path)
{
    if (answer.status == 401 || answer.status == 403)
        return client.status_error(answer.status, path);
    Json json;
    if (Json::parse(answer.body, &json) && json["detail"].kind == Json::string)
        return json["detail"].text + " (" + std::to_string(answer.status) + ")";
    return client.status_error(answer.status);
}

/* What tells this console apart from others of the user: made once, kept beside the store's
 * folder. */
std::string console_id(const std::string &folder)
{
    const std::string file = files::parent(folder) + "/console-id";
    std::string id;
    files::read(file, &id);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' '))
        id.pop_back();
    if (id.size() == 16)
        return id;
    /* Unlike any other console's is all it needs to be: the time, to the nanosecond, mixed. */
    uint64_t mixed = uint64_t(std::chrono::system_clock::now().time_since_epoch().count()) ^
                     (uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) << 17);
    mixed = std::mt19937_64(mixed)();
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", (unsigned long long)mixed);
    id = out;
    files::make_folders(files::parent(file));
    (void)files::write(file, id + "\n");
    return id;
}

std::string id_of(const Json &asset)
{
    const int64_t id = number(asset, "id");
    return id > 0 ? std::to_string(id) : std::string();
}

/* A state's version: RomM keeps no hash of it, so its id and when it was last written. */
std::string state_version(const Json &state)
{
    return id_of(state) + "@" + text(state, "updated_at");
}

/* A save's version: its contents' hash, which RomM keeps. */
std::string save_version(const Json &save)
{
    const std::string hash = text(save, "content_hash");
    return !hash.empty() ? hash : id_of(save) + "@" + text(save, "updated_at");
}

/* What the save sync asks for: the games, their save data, this console as a device, and the
 * user's name for the menu. */
const char *const scopes[] = {"platforms.read", "roms.read",     "assets.read", "assets.write",
                              "devices.read",   "devices.write", "me.read"};

class RommSaves final : public SaveStore
{
  public:
    RommSaves(std::unique_ptr<Client> client, std::string folder)
        : client_(std::move(client)), folder_(std::move(folder))
    {
    }

    std::string address() const override
    {
        return client_->url();
    }
    std::string user() const override
    {
        return user_;
    }
    bool too_old(std::string *version, std::string *needed) const override
    {
        if (too_old_.empty())
            return false;
        *version = too_old_;
        *needed = minimum_version;
        return true;
    }

    bool prepare(const Stopped &stopped, std::string *error) override
    {
        stopped_ = stopped; /* every transfer of this sync asks it */
        std::string body;
        if (!client_->get("/api/heartbeat", stopped, &body, error))
            return false;
        Json heartbeat;
        Json::parse(body, &heartbeat);
        const std::string version = text(heartbeat["SYSTEM"], "VERSION");
        too_old_.clear();
        if (!can_sync(version))
        {
            too_old_ = version;
            *error = "RomM " + version + " is older than the save sync takes: it needs RomM " +
                     minimum_version + " or newer";
            return false;
        }
        /* Who it is, for the menu and for what was in step with whom (save_sync.cpp): a token
         * may not be allowed to say (the scope me.read), always the same way. A server that
         * does not answer now ends the sync instead of making it someone else. */
        Client::Answer answer;
        if (!client_->send(nullptr, "/api/users/me", {}, &answer, error, stopped_))
            return false;
        if (answer.status >= 500)
        {
            *error = detail(*client_, answer, "/api/users/me");
            return false;
        }
        if (answer.status == 200)
        {
            Json me;
            Json::parse(answer.body, &me);
            user_ = text(me, "username");
        }
        return device(false, error);
    }

    bool games(std::vector<SourceGame> *games, const Stopped &stopped, std::string *error) override
    {
        return client_->games(games, stopped, error);
    }

    bool names(const SourceGame &game, SaveKind kind, const std::string &emulator,
               std::vector<std::string> *names, std::string *error) override
    {
        names->clear();
        Json list;
        if (!assets(game, kind, &list, error))
            return false;
        for (const Json &asset : list.items)
            if (text(asset, "emulator") == emulator && !text(asset, "file_name").empty())
                names->push_back(text(asset, "file_name"));
        return true;
    }

    bool compare(const SourceGame &game, const LocalSave &local, SavePlan *plan,
                 std::string *error) override
    {
        *plan = SavePlan{};
        if (local.kind == SaveKind::save && !negotiate(game, local, plan, error))
            return false;
        Json list;
        if (!assets(game, local.kind, &list, error))
            return false;
        /* The server's copy: the newest of the slot (a save), the state of the file's name. */
        const Json *newest = nullptr;
        for (const Json &asset : list.items)
        {
            if (asset.kind != Json::object || text(asset, "emulator") != local.emulator)
                continue;
            if (local.kind == SaveKind::state && text(asset, "file_name") != local.name)
                continue;
            const auto key = [](const Json &a)
            { return std::make_pair(parse_time(text(a, "updated_at")), number(a, "id")); };
            if (newest == nullptr || key(asset) > key(*newest))
                newest = &asset;
        }
        if (newest == nullptr)
        {
            plan->action = local.present ? SaveAction::upload : SaveAction::none;
            return true;
        }
        plan->remote = id_of(*newest);
        plan->hash = text(*newest, "content_hash");
        plan->version =
            local.kind == SaveKind::save ? save_version(*newest) : state_version(*newest);
        plan->updated = parse_time(text(*newest, "updated_at"));
        plan->size = uint64_t(std::max<int64_t>(number(*newest, "file_size_bytes"), 0));
        if (!local.present)
            plan->action = SaveAction::download;
        else if (!plan->hash.empty() && local.hash == plan->hash)
            plan->action = SaveAction::none;
        else
            plan->action = SaveAction::conflict; /* the save sync tells which side changed */
        if (plan->action == SaveAction::conflict && local.kind == SaveKind::save)
            describe(plan);
        return true;
    }

    bool upload(const SourceGame &game, const LocalSave &local, const std::string &path,
                bool overwrite, bool *newer, std::string *version, std::string *error) override
    {
        *newer = false;
        Client::Answer answer;
        std::string query;
        if (local.kind == SaveKind::save)
        {
            query = "/api/saves?rom_id=" + game.id + "&emulator=" + escape(local.emulator) +
                    "&slot=" + slot + "&device_id=" + escape(device_) +
                    "&overwrite=" + (overwrite ? "true" : "false") +
                    "&autocleanup=true&autocleanup_limit=" + std::to_string(kept) +
                    "&content_hash=" + local.hash;
            if (session_ > 0)
                query += "&session_id=" + std::to_string(session_);
        }
        else
            /* A state of the same name is written over: the server keeps one of each. */
            query = "/api/states?rom_id=" + game.id + "&emulator=" + escape(local.emulator);
        const char *field = local.kind == SaveKind::save ? "saveFile" : "stateFile";
        if (!client_->upload("POST", query, field, path, local.name, &answer, error, stopped_))
        {
            ++failed_;
            return false;
        }
        if (answer.status != 200 && answer.status != 201)
        {
            ++failed_;
            /* Another device's version came in since this console last had the slot. */
            if (answer.status == 409)
            {
                *newer = true;
                *error = "The server has newer save data than this console last had";
            }
            else if (answer.status == 413)
                *error = "The save data is too large for the server";
            else
                *error = detail(*client_, answer, query.substr(0, query.find('?')));
            return false;
        }
        Json made;
        Json::parse(answer.body, &made);
        *version = local.kind == SaveKind::save ? save_version(made) : state_version(made);
        if (local.kind == SaveKind::save && text(made, "content_hash").empty())
            *version = local.hash; /* what the server will say of it */
        ++done_;
        return true;
    }

    bool download(const SourceGame &, const LocalSave &local, const SavePlan &plan,
                  const std::string &path, std::string *error) override
    {
        if (plan.remote.empty())
        {
            *error = "The server has no such file of the game";
            return false;
        }
        const std::string what = local.kind == SaveKind::save
                                     ? "/api/saves/" + plan.remote +
                                           "/content?device_id=" + escape(device_) +
                                           "&optimistic=false"
                                     : "/api/states/" + plan.remote + "/content";
        if (!client_->download(what, path, error, stopped_))
        {
            ++failed_;
            return false;
        }
        /* A save is taken as this console's version once it is in place (finish). */
        if (local.kind == SaveKind::save)
        {
            confirm_ = plan.remote;
            confirm_hash_ = plan.hash;
        }
        ++done_;
        return true;
    }

    void finish(bool done) override
    {
        Client::Answer answer;
        std::string error;
        if (done && !confirm_.empty())
        {
            Json payload = Json::record();
            payload.set("device_id", Json::of(device_));
            if (!confirm_hash_.empty())
                payload.set("content_hash", Json::of(confirm_hash_));
            if (!client_->send("POST", "/api/saves/" + confirm_ + "/downloaded", payload.write(),
                               &answer, &error) ||
                answer.status != 200)
                done = false;
        }
        if (!done && failed_ == 0)
            failed_ = 1;
        if (session_ > 0)
        {
            Json payload = Json::record();
            payload.set("operations_completed", Json::of(double(done_)));
            payload.set("operations_failed", Json::of(double(failed_)));
            (void)client_->send("POST",
                                "/api/sync/sessions/" + std::to_string(session_) + "/complete",
                                payload.write(), &answer, &error);
        }
        session_ = 0;
        done_ = failed_ = 0;
        confirm_.clear();
        confirm_hash_.clear();
    }

  private:
    /* The game's saves of the slot, or its states. */
    bool assets(const SourceGame &game, SaveKind kind, Json *list, std::string *error) const
    {
        const std::string path = kind == SaveKind::save
                                     ? "/api/saves?rom_id=" + game.id + "&slot=" + slot
                                     : "/api/states?rom_id=" + game.id;
        Client::Answer answer;
        if (!client_->send(nullptr, path, {}, &answer, error, stopped_))
            return false;
        if (answer.status != 200)
        {
            *error = detail(*client_, answer, path.substr(0, path.find('?')));
            return false;
        }
        if (!Json::parse(answer.body, list) || list->kind != Json::array)
        {
            *error = "The server's list of save data cannot be read";
            return false;
        }
        return true;
    }

    /* The sync session RomM shows with the device, and its reason for the log. What to do is
     * not taken from its operations: RomM 5.3 pairs the console's save with another emulator's
     * in the same slot, and compares times when it no longer knows what the console had. */
    bool negotiate(const SourceGame &game, const LocalSave &local, SavePlan *plan,
                   std::string *error)
    {
        Client::Answer answer;
        for (int attempt = 0;; ++attempt)
        {
            Json payload = Json::record();
            payload.set("device_id", Json::of(device_));
            Json &saves = payload.set("saves", Json::list());
            if (local.present)
            {
                Json &save = saves.push(Json::record());
                save.set("rom_id", Json::of(double(std::atoll(game.id.c_str()))));
                save.set("file_name", Json::of(local.name));
                save.set("slot", Json::of(slot));
                save.set("emulator", Json::of(local.emulator));
                save.set("content_hash", Json::of(local.hash));
                save.set("updated_at", Json::of(format_time(local.updated)));
                save.set("file_size_bytes", Json::of(double(local.size)));
            }
            payload.set("rom_ids", Json::list())
                .push(Json::of(double(std::atoll(game.id.c_str()))));
            payload.set("emulators", Json::list()).push(Json::of(local.emulator));
            if (!client_->send("POST", "/api/sync/negotiate", payload.write(), &answer, error,
                               stopped_))
                return false;
            /* The device was taken off the server (by its user, in RomM's settings): once
             * more as a new one. */
            if (answer.status == 404 && attempt == 0)
            {
                if (!device(true, error))
                    return false;
                continue;
            }
            break;
        }
        if (answer.status != 200)
        {
            *error = detail(*client_, answer, "/api/sync");
            return false;
        }
        Json result;
        if (!Json::parse(answer.body, &result) || result.kind != Json::object)
        {
            *error = "The server's answer cannot be read";
            return false;
        }
        session_ = number(result, "session_id");
        for (const Json &operation : result["operations"].items)
            if (operation["rom_id"].str() == game.id && text(operation, "slot") == slot &&
                text(operation, "emulator") == local.emulator)
                plan->reason = text(operation, "reason");
        return true;
    }

    /* This console as a device of the user: the one registered before, else a new one. again:
     * the server forgot the one it had. */
    bool device(bool again, std::string *error)
    {
        const std::string state_file = folder_ + "/romm-device.json";
        std::string content;
        Json state;
        if (!again && files::read(state_file, &content) && Json::parse(content, &state) &&
            text(state, "url") == client_->url() && text(state, "user") == user_ &&
            !text(state, "device_id").empty())
        {
            device_ = text(state, "device_id");
            return true;
        }
        Json payload = Json::record();
        payload.set("name", Json::of("PS5 RetroArch"));
        payload.set("platform", Json::of("ps5"));
        payload.set("client", Json::of("PS5-RetroArch"));
        payload.set("hostname", Json::of("ps5-retroarch-" + console_id(folder_)));
        payload.set("sync_mode", Json::of("api"));
        payload.set("allow_existing", Json::of(true));
        Client::Answer answer;
        if (!client_->send("POST", "/api/devices", payload.write(), &answer, error, stopped_))
            return false;
        Json made;
        Json::parse(answer.body, &made);
        if ((answer.status != 200 && answer.status != 201) || text(made, "device_id").empty())
        {
            *error = detail(*client_, answer, "/api/devices");
            return false;
        }
        device_ = text(made, "device_id");
        Json kept_state = Json::record();
        kept_state.set("url", Json::of(client_->url()));
        kept_state.set("user", Json::of(user_));
        kept_state.set("device_id", Json::of(device_));
        if (!files::make_folders(folder_) || !files::write(state_file, kept_state.write(true)))
        {
            *error = "Cannot write " + state_file;
            return false;
        }
        return true;
    }

    /* The device that made the server's copy, by its name in RomM, for the player's choice. */
    void describe(SavePlan *plan) const
    {
        std::string body, error;
        if (!client_->get("/api/saves/" + plan->remote + "?device_id=" + escape(device_), stopped_,
                          &body, &error))
            return;
        Json save;
        if (!Json::parse(body, &save))
            return;
        const std::string origin = text(save, "origin_device_id");
        Json found;
        if (!origin.empty() &&
            client_->get("/api/devices/" + escape(origin), stopped_, &body, &error) &&
            Json::parse(body, &found))
            plan->device = text(found, "name");
    }

    const std::unique_ptr<Client> client_;
    const std::string folder_;
    std::string user_;
    std::string too_old_; /* the server's version, when it is older than minimum_version */
    Stopped stopped_;     /* from prepare: a game starts, the title closes */
    std::string device_;
    int64_t session_ = 0;
    int done_ = 0;
    int failed_ = 0;
    std::string confirm_; /* the server's save downloaded, to be taken as this console's */
    std::string confirm_hash_;
};
/* An entry asked of without a sign-in: its address only. */
std::unique_ptr<Client> unsigned_client(const Json &settings, std::string *error)
{
    Json entry = settings.kind == Json::object ? settings : Json::record();
    for (const char *key : {"token", "username", "password"})
        entry.erase(key);
    return Client::make(entry, "save-sync.json", error);
}

bool pair_start(const Json &settings, const std::string &folder, PairingStart *start,
                std::string *error, const Stopped &stopped)
{
    std::unique_ptr<Client> client = unsigned_client(settings, error);
    if (!client)
        return false;
    Json payload = Json::record();
    payload.set("client_device_identifier", Json::of("ps5-retroarch-" + console_id(folder)));
    payload.set("name", Json::of("PS5 RetroArch"));
    payload.set("client", Json::of("PS5-RetroArch"));
    payload.set("platform", Json::of("ps5"));
    Json &wanted = payload.set("requested_scopes", Json::list());
    for (const char *scope : scopes)
        wanted.push(Json::of(scope));
    Client::Answer answer;
    if (!client->send("POST", "/api/auth/device/init", payload.write(), &answer, error, stopped))
        return false;
    if (answer.status == 404 || answer.status == 405)
    {
        *error = std::string("Pairing needs RomM ") + minimum_version + " or newer";
        return false;
    }
    Json made;
    Json::parse(answer.body, &made);
    if ((answer.status != 200 && answer.status != 201) || text(made, "device_code").empty())
    {
        *error = detail(*client, answer, "/api/auth");
        return false;
    }
    start->device_code = text(made, "device_code");
    start->user_code = text(made, "user_code");
    start->address = client->url() + text(made, "verification_path_complete");
    /* RomM's default when a server does not say (ten minutes). */
    start->expires_in = number(made, "expires_in") > 0 ? int(number(made, "expires_in")) : 600;
    start->interval = std::max(1, int(number(made, "interval")));
    return true;
}

PairingResult pair_poll(const Json &settings, const std::string &folder, const PairingStart &start,
                        const Stopped &stopped)
{
    PairingResult result;
    std::unique_ptr<Client> client = unsigned_client(settings, &result.error);
    if (!client)
        return result;
    Json payload = Json::record();
    payload.set("device_code", Json::of(start.device_code));
    Client::Answer answer;
    if (!client->send("POST", "/api/auth/device/token", payload.write(), &answer, &result.error,
                      stopped))
        return result;
    Json body;
    Json::parse(answer.body, &body);
    if (answer.status == 400)
    {
        const std::string why = text(body, "detail");
        if (why == "authorization_pending" || why == "slow_down")
        {
            result.state = PairingState::pending;
            /* slow_down: 5 s more than the interval used so far. */
            result.interval = start.interval + (why == "slow_down" ? 5 : 0);
        }
        else if (why == "access_denied")
            result.state = PairingState::denied;
        else if (why == "expired_token")
            result.state = PairingState::expired;
        else
            result.error = detail(*client, answer, "/api/auth");
        return result;
    }
    if (answer.status != 200 || text(body, "access_token").empty())
    {
        result.error = detail(*client, answer, "/api/auth");
        return result;
    }
    /* Signed in: the token is bound to the device RomM made for this console, which the save
     * sync then is (RommSaves::device finds it kept), and says whose it is. */
    const std::string token = text(body, "access_token");
    Json entry = Json::record();
    entry.set("token", Json::of(token));
    entry.set("url", Json::of(client->url()));
    std::string ignored;
    std::unique_ptr<Client> signed_in = Client::make(entry, "save-sync.json", &ignored);
    Client::Answer me;
    if (signed_in && signed_in->send(nullptr, "/api/users/me", {}, &me, &ignored, stopped) &&
        me.status == 200)
    {
        Json user;
        Json::parse(me.body, &user);
        result.user = text(user, "username");
    }
    Json kept_state = Json::record();
    kept_state.set("url", Json::of(client->url()));
    kept_state.set("user", Json::of(result.user));
    kept_state.set("device_id", Json::of(text(body, "device_id")));
    files::make_folders(folder);
    (void)files::write(folder + "/romm-device.json", kept_state.write(true));
    result.state = PairingState::approved;
    result.entry = Json::record();
    result.entry.set("type", Json::of("romm"));
    result.entry.set("url", Json::of(client->url()));
    result.entry.set("token", Json::of(token));
    return result;
}
} // namespace

const Pairing pairing{pair_start, pair_poll};

std::unique_ptr<SaveStore> make_saves(const Json &settings, const std::string &folder,
                                      std::string *error)
{
    std::unique_ptr<Client> client = Client::make(settings, "save-sync.json", error);
    if (!client)
        return nullptr;
    if (client->authorization().empty())
    {
        *error = "No sign-in (\"token\", or \"username\" and \"password\") in save-sync.json";
        return nullptr;
    }
    return std::unique_ptr<SaveStore>(new RommSaves(std::move(client), folder));
}

int64_t parse_time(const std::string &text_)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, used = 0;
    if (std::sscanf(text_.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute,
                    &second, &used) != 6)
        return 0;
    size_t at = size_t(used);
    if (at < text_.size() && text_[at] == '.')
        for (++at; at < text_.size() && text_[at] >= '0' && text_[at] <= '9';)
            ++at;
    int64_t offset = 0;
    if (at < text_.size() && (text_[at] == '+' || text_[at] == '-'))
    {
        int hours = 0, minutes = 0;
        if (std::sscanf(text_.c_str() + at + 1, "%2d:%2d", &hours, &minutes) != 2)
            return 0;
        offset = (text_[at] == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
    }
    /* Days since 1970 of a date (Howard Hinnant's days_from_civil). */
    const int y = year - (month <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int year_of_era = y - era * 400;
    const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t days = int64_t(era) * 146097 + day_of_era - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second - offset;
}

std::string format_time(int64_t seconds)
{
    const std::time_t time = std::time_t(seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char out[32];
    std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return out;
}

bool can_sync(const std::string &version)
{
    int have[3] = {}, need[3] = {};
    if (std::sscanf(version.c_str(), "%d.%d.%d", &have[0], &have[1], &have[2]) < 2)
        return true;
    (void)std::sscanf(minimum_version, "%d.%d.%d", &need[0], &need[1], &need[2]);
    return !std::lexicographical_compare(have, have + 3, need, need + 3);
}
} // namespace ps5::remote::romm
