/* PS5 RetroArch - a RomM server as its backends talk to it (src/remote/romm/romm_client.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_client.h"

#include "../files.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>

namespace ps5::remote::romm
{
namespace
{
/* Games asked for per page of /api/roms, and the most pages read. */
constexpr int page_size = 200;
constexpr int most_pages = 500;
/* The most a JSON answer may be. */
constexpr uint64_t most_json = 64u << 20;

/* RomM's fields with a game's id at a metadata provider, and the provider's name for
 * SourceGame::ids. */
const std::pair<const char *, const char *> providers[] = {
    {"igdb_id", "igdb"},
    {"ss_id", "screenscraper"},
    {"moby_id", "mobygames"},
    {"launchbox_id", "launchbox"},
    {"hasheous_id", "hasheous"},
    {"tgdb_id", "thegamesdb"},
    {"ra_id", "retroachievements"},
    {"sgdb_id", "steamgriddb"},
    {"hltb_id", "howlongtobeat"},
    {"flashpoint_id", "flashpoint"},
    {"libretro_id", "libretro"},
};

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

std::string text(const Json &object, const char *key)
{
    return object[key].kind == Json::string ? object[key].text : std::string();
}

/* Whether RomM's hashes of a game file are of what it unpacks to: an archive's are of the
 * files in it (archive_members lists them), and a compressed file's of what it decompresses
 * to (backend/handler/filesystem/roms_handler.py), so they match hash databases and not the
 * bytes downloaded. */
bool unpacked_hashes(const Json &entry, const std::string &name)
{
    if (entry["archive_members"].kind == Json::array)
        return true;
    const std::string file = lower(name);
    for (const char *extension :
         {".zip", ".7z", ".rar", ".tar", ".tgz", ".tbz2", ".txz", ".gz", ".bz2", ".xz"})
    {
        const size_t length = std::strlen(extension);
        if (file.size() > length && file.compare(file.size() - length, length, extension) == 0)
            return true;
    }
    return false;
}

/* An id as text: a number, or a string that is not empty or "0". */
std::string id_text(const Json &value)
{
    const std::string id = value.str();
    return id.empty() || id == "0" ? std::string() : id;
}

} // namespace

ps5_scraper::Http &Client::http()
{
    static thread_local ps5_scraper::Http client("PS5-RetroArch/remote");
    return client;
}

std::unique_ptr<Client> Client::make(const Json &settings, const std::string &file,
                                     std::string *error)
{
    const std::string url = normal_url(text(settings, "url"));
    if (url.empty())
    {
        *error = "No server address (\"url\") in " + file;
        return nullptr;
    }
    std::string authorization;
    const std::string token = text(settings, "token");
    const std::string user = text(settings, "username");
    if (!token.empty())
        authorization = "Bearer " + token;
    else if (!user.empty())
        authorization = ps5_scraper::basic_authorization(user, text(settings, "password"));
    std::vector<std::string> platforms;
    for (const Json &platform : settings["platforms"].items)
        if (!platform.str().empty())
            platforms.push_back(lower(platform.str()));
    return std::unique_ptr<Client>(new Client(url, authorization, platforms, file));
}

std::string Client::status_error(int status, const std::string &path) const
{
    if (status == 401)
        return "RomM did not accept the token or password in " + file_;
    if (status == 403)
    {
        /* A refusal: both a wrong token or password and a missing scope are named, with the
         * scope a client API token needs for what was asked. */
        static const std::pair<const char *, const char *> scopes[] = {
            {"/api/platforms", "platforms.read"},
            {"/api/roms", "roms.read"},
            {"/api/saves", "assets.read and assets.write"},
            {"/api/states", "assets.read and assets.write"},
            {"/api/sync", "assets.read and devices.read"},
            {"/api/devices", "devices.write"},
            {"/api/firmware", "firmware.read"}};
        for (const auto &scope : scopes)
            if (path.rfind(scope.first, 0) == 0)
                return "RomM did not accept the token or password in " + file_ +
                       ", or the token lacks the scope " + scope.second;
        return "RomM did not accept the token or password in " + file_ +
               ", or did not let it do this";
    }
    if (status == 404)
        return "The server has no such page (is the address in " + file_ + " RomM's?)";
    return "The server answered with status " + std::to_string(status);
}

bool Client::get(const std::string &path, const Stopped &stopped, std::string *body,
                 std::string *error, unsigned timeout) const
{
    ps5_scraper::Request request;
    request.url = url_ + path;
    if (!authorization_.empty())
        request.headers.emplace_back("Authorization", authorization_);
    request.limit = most_json;
    request.stopped = stopped;
    request.timeout = timeout;
    const ps5_scraper::Response response = http().send(request);
    if (response.cancelled)
        *error = "Stopped";
    else if (response.too_large)
        *error = "The server's answer is too large";
    else if (response.status == 0)
        *error = response.error.empty() ? "The server did not answer" : response.error;
    else if (response.status != 200)
        *error = status_error(response.status, path);
    else
    {
        *body = response.body;
        return true;
    }
    return false;
}

bool Client::send(const char *method, const std::string &path, const std::string &json,
                  Answer *answer, std::string *error, const Stopped &stopped) const
{
    ps5_scraper::Request request;
    if (method != nullptr && *method != '\0')
        request.method = method;
    request.url = url_ + path;
    if (!authorization_.empty())
        request.headers.emplace_back("Authorization", authorization_);
    if (!json.empty())
    {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = json;
    }
    request.limit = most_json;
    request.stopped = stopped;
    const ps5_scraper::Response response = http().send(request);
    answer->status = response.status;
    answer->body = response.body;
    if (response.cancelled)
        *error = "Stopped";
    else if (response.too_large)
        *error = "The server's answer is too large";
    else if (response.status == 0)
        *error = response.error.empty() ? "The server did not answer" : response.error;
    else
        return true;
    return false;
}

bool Client::upload(const char *method, const std::string &path, const std::string &field,
                    const std::string &file, const std::string &name, Answer *answer,
                    std::string *error, const Stopped &stopped) const
{
    std::string data;
    if (!files::read(file, &data, most_json))
    {
        *error = "Cannot read " + file;
        return false;
    }
    ps5_scraper::Request request;
    request.method = method;
    request.url = url_ + path;
    if (!authorization_.empty())
        request.headers.emplace_back("Authorization", authorization_);
    std::string type;
    request.body = ps5_scraper::form_file(field, name, data, &type);
    request.headers.emplace_back("Content-Type", type);
    request.limit = most_json;
    request.stopped = stopped;
    const ps5_scraper::Response response = http().send(request);
    answer->status = response.status;
    answer->body = response.body;
    if (response.cancelled)
        *error = "Stopped";
    else if (response.status == 0)
        *error = response.error.empty() ? "The server did not answer" : response.error;
    else
        return true;
    return false;
}

bool Client::download(const std::string &path, const std::string &file, std::string *error,
                      const Stopped &stopped) const
{
    std::string body;
    ps5_scraper::Request request;
    request.url = url_ + path;
    if (!authorization_.empty())
        request.headers.emplace_back("Authorization", authorization_);
    request.limit = most_json;
    request.stopped = stopped;
    const ps5_scraper::Response response = http().send(request);
    if (response.cancelled)
        *error = "Stopped";
    else if (response.too_large)
        *error = "The server's file is too large";
    else if (response.status == 0)
        *error = response.error.empty() ? "The server did not answer" : response.error;
    else if (response.status != 200)
        *error = status_error(response.status, path);
    else if (!files::make_folders(files::parent(file)) || !files::write(file, response.body))
        *error = "Cannot write " + file;
    else
        return true;
    return false;
}

bool Client::fetch(const std::string &path_or_url, uint64_t limit, const Stopped &stopped,
                   std::string *body) const
{
    /* The server's own files may need the sign-in; another site's (IGDB) never gets it. */
    const bool own = path_or_url.rfind("http://", 0) != 0 && path_or_url.rfind("https://", 0) != 0;
    ps5_scraper::Request request;
    request.url = own ? url_ + path_or_url : path_or_url;
    if (own && !authorization_.empty())
        request.headers.emplace_back("Authorization", authorization_);
    request.limit = limit;
    request.stopped = stopped;
    const ps5_scraper::Response response = http().send(request);
    if (response.status != 200 || !response.error.empty() || response.cancelled ||
        response.too_large)
        return false;
    *body = response.body;
    return true;
}

bool Client::check_version(const Stopped &stopped, std::string *error, unsigned timeout) const
{
    {
        std::lock_guard<std::mutex> guard(version_lock_);
        if (version_ok_)
            return true;
    }
    std::string body;
    if (!get("/api/heartbeat", stopped, &body, error, timeout))
        return false;
    Json heartbeat;
    Json::parse(body, &heartbeat);
    const std::string version = text(heartbeat["SYSTEM"], "VERSION");
    std::lock_guard<std::mutex> guard(version_lock_);
    version_ = version;
    version_ok_ = new_enough(version);
    too_old_ = version_ok_ ? "" : version;
    if (!version_ok_)
        *error = "RomM " + version + " is too old: it needs RomM " + minimum_version + " or newer";
    return version_ok_;
}

std::string Client::version() const
{
    std::lock_guard<std::mutex> guard(version_lock_);
    return version_;
}

std::string Client::too_old() const
{
    std::lock_guard<std::mutex> guard(version_lock_);
    return too_old_;
}

bool Client::stream(const std::string &path, uint64_t offset, Receiver &receiver,
                    std::string *error) const
{
    for (;;)
    {
        ps5_scraper::Request request;
        request.url = url_ + path;
        if (!authorization_.empty())
            request.headers.emplace_back("Authorization", authorization_);
        if (offset > 0)
            request.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-");
        request.stopped = [&receiver] { return receiver.stopped(); };
        int status = 0;
        /* 206: from the offset asked for; 200: the whole file. Anything else is an error page. */
        request.begin = [&](int code)
        {
            status = code;
            return (code == 200 || code == 206) && receiver.begin(code == 200);
        };
        request.sink = [&receiver](const char *data, size_t size)
        { return receiver.take(data, size); };
        const ps5_scraper::Response response = http().send(request);
        /* 416: the file on the server is shorter than what was begun: it starts again. */
        if (status == 416 && offset > 0 && !receiver.stopped())
        {
            offset = 0;
            continue;
        }
        if (status != 200 && status != 206)
        {
            *error = status == 0
                         ? (response.error.empty() ? "The server did not answer" : response.error)
                         : status_error(status, path);
            return false;
        }
        if (!response.error.empty() || response.cancelled)
        {
            *error = !response.error.empty() ? response.error : "The download stopped";
            return false;
        }
        return true;
    }
}

bool Client::platform_filter(const Stopped &stopped, unsigned timeout, std::string *filter,
                             std::string *error) const
{
    std::string body;
    if (!get("/api/platforms", stopped, &body, error, timeout))
        return false;
    Json platforms;
    if (!Json::parse(body, &platforms) || platforms.kind != Json::array)
    {
        *error =
            "The server's platform list cannot be read (is the address in " + file_ + " RomM's?)";
        return false;
    }
    for (const Json &item : platforms.items)
        for (const std::string &wanted : platforms_)
            if (lower(text(item, "slug")) == wanted || lower(text(item, "fs_slug")) == wanted)
                *filter += "&platform_ids=" + id_text(item["id"]);
    if (filter->empty())
    {
        *error = "The server has none of the platforms " + file_ + " names";
        return false;
    }
    return true;
}

bool Client::games(std::vector<SourceGame> *games, const Stopped &stopped, std::string *error,
                   unsigned timeout) const
{
    if (!check_version(stopped, error, timeout))
        return false;
    std::string filter;
    if (!platforms_.empty() && !platform_filter(stopped, timeout, &filter, error))
        return false;
    games->clear();
    /* From where the last page ended: a server may hand out fewer games a page than asked for. */
    size_t offset = 0;
    for (int page = 0; page < most_pages; ++page)
    {
        std::string body;
        if (!get("/api/roms?with_files=true&limit=" + std::to_string(page_size) +
                     "&offset=" + std::to_string(offset) + filter +
                     "&order_by=name&order_dir=asc&with_char_index=false"
                     "&with_filter_values=false&with_rom_id_index=false",
                 stopped, &body, error, timeout))
            return false;
        size_t listed = 0, total = 0;
        if (!parse_page(body, games, &listed, &total))
        {
            *error = "The server's game list cannot be read";
            return false;
        }
        offset += listed;
        if (listed == 0 || offset >= total)
            break;
    }
    return true;
}

std::string normal_url(std::string url)
{
    const size_t first = url.find_first_not_of(" \t\r\n");
    url = first == std::string::npos ? "" : url.substr(first);
    while (!url.empty() && (url.back() == '/' || url.back() == ' ' || url.back() == '\t' ||
                            url.back() == '\r' || url.back() == '\n'))
        url.pop_back();
    if (url.empty())
        return url;
    if (url.find("://") == std::string::npos)
        url = "http://" + url;
    const std::string scheme = lower(url.substr(0, url.find("://")));
    if (scheme != "http" && scheme != "https")
        return "";
    return scheme + url.substr(url.find("://"));
}

const std::vector<std::string> &console_scopes()
{
    static const std::vector<std::string> scopes = {
        "platforms.read", "roms.read",    "firmware.read", "assets.read",
        "assets.write",   "devices.read", "devices.write", "me.read"};
    return scopes;
}

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

bool new_enough(const std::string &version)
{
    int have[3] = {}, need[3] = {};
    if (std::sscanf(version.c_str(), "%d.%d.%d", &have[0], &have[1], &have[2]) < 2)
        return true;
    (void)std::sscanf(minimum_version, "%d.%d.%d", &need[0], &need[1], &need[2]);
    return !std::lexicographical_compare(have, have + 3, need, need + 3);
}

bool parse_firmware(const std::string &firmware, const std::string &platforms,
                    std::vector<FirmwareFile> *files)
{
    Json list, known;
    if (!Json::parse(firmware, &list) || list.kind != Json::array ||
        !Json::parse(platforms, &known) || known.kind != Json::array)
        return false;
    files->clear();
    for (const Json &item : list.items)
    {
        if (item.kind != Json::object || item["missing_from_fs"].yes() ||
            id_text(item["id"]).empty() || text(item, "file_name").empty())
            continue;
        FirmwareFile file;
        file.id = id_text(item["id"]);
        file.name = text(item, "file_name");
        const std::string platform = id_text(item["platform_id"]);
        for (const Json &entry : known.items)
            if (!platform.empty() && id_text(entry["id"]) == platform)
                for (const char *field : {"fs_slug", "slug", "display_name", "name"})
                    if (!text(entry, field).empty())
                        file.systems.push_back(text(entry, field));
        file.size = item["file_size_bytes"].whole();
        file.crc32 = text(item, "crc_hash");
        file.md5 = text(item, "md5_hash");
        file.sha1 = text(item, "sha1_hash");
        file.verified = item["is_verified"].yes();
        files->push_back(std::move(file));
    }
    return true;
}

bool parse_page(const std::string &text_, std::vector<SourceGame> *games, size_t *listed,
                size_t *total)
{
    if (listed)
        *listed = 0;
    if (total)
        *total = 0;
    Json page;
    if (!Json::parse(text_, &page) || page.kind != Json::object ||
        page["items"].kind != Json::array)
        return false;
    if (total)
        *total = size_t(page["total"].whole());
    if (listed)
        *listed = page["items"].items.size();
    for (const Json &item : page["items"].items)
    {
        if (item.kind != Json::object || item["missing_from_fs"].yes() ||
            id_text(item["id"]).empty())
            continue;
        SourceGame game;
        game.id = id_text(item["id"]);
        game.name = text(item, "name");
        if (game.name.empty())
            game.name = text(item, "fs_name_no_ext");
        if (game.name.empty())
            game.name = text(item, "fs_name");
        game.identified = item["is_identified"].yes();
        /* Its platform, as RomM's folder for it, its slug and its names say. */
        for (const char *field :
             {"platform_fs_slug", "platform_slug", "platform_display_name", "platform_name"})
            if (!text(item, field).empty())
                game.systems.push_back(text(item, field));
        game.folder = text(item, "fs_name_no_ext");
        if (game.folder.empty())
            game.folder = text(item, "fs_name");
        /* The ids of each metadata provider RomM matched it at. */
        for (const auto &provider : providers)
            if (const std::string id = id_text(item[provider.first]); !id.empty())
                game.ids[provider.second] = id;
        /* RomM says what each file is (its category): one of none is the game, the rest
         * (manuals, patches, DLC...) stays on the server. A file's path inside the game is the
         * part of its path below the ROM's own. */
        const std::string root = text(item, "fs_path") + "/" + text(item, "fs_name") + "/";
        for (const Json &entry : item["files"].items)
        {
            const std::string category = lower(text(entry, "category"));
            if (id_text(entry["id"]).empty() || (!category.empty() && category != "game"))
                continue;
            SourceFile file;
            file.id = id_text(entry["id"]);
            const std::string full = text(entry, "full_path");
            file.name =
                full.rfind(root, 0) == 0 ? full.substr(root.size()) : text(entry, "file_name");
            file.size = entry["file_size_bytes"].whole();
            file.crc32 = text(entry, "crc_hash");
            file.md5 = text(entry, "md5_hash");
            file.sha1 = text(entry, "sha1_hash");
            file.unpacked = unpacked_hashes(entry, file.name);
            game.files.push_back(std::move(file));
        }
        std::string cover = text(item, "path_cover_small");
        if (cover.empty())
            cover = text(item, "path_cover_large");
        if (!cover.empty() && cover.front() != '/' && cover.rfind("http", 0) != 0)
            cover = "/assets/romm/resources/" + cover;
        game.cover = !cover.empty() ? cover : text(item, "url_cover");
        games->push_back(std::move(game));
    }
    return true;
}
} // namespace ps5::remote::romm
