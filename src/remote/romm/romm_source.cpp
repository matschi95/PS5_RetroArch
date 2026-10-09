/* PS5 RetroArch - the RomM backend of the download sources (src/remote/romm/romm_source.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_source.h"

#include "../../scraper_http.h"

#include <algorithm>
#include <utility>

namespace ps5::remote::romm
{
namespace
{
/* Games asked for per page of /api/roms, and the most pages read. */
constexpr int page_size = 200;
constexpr int most_pages = 500;
/* The most a JSON answer and a cover may be. */
constexpr uint64_t most_json = 64u << 20;
constexpr uint64_t most_cover = 8u << 20;

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

/* An id as text: a number, or a string that is not empty or "0". */
std::string id_text(const Json &value)
{
    const std::string id = value.str();
    return id.empty() || id == "0" ? std::string() : id;
}

/* The HTTP client of the thread asking: each thread keeps its connections. */
ps5_scraper::Http &client()
{
    static thread_local ps5_scraper::Http http("PS5-RetroArch/remote");
    return http;
}

std::string status_error(int status)
{
    if (status == 401)
        return "RomM did not accept the token or password in sources.json";
    if (status == 403)
        return "RomM did not accept the token or password in sources.json, or the token lacks "
               "the scope roms.read";
    if (status == 404)
        return "The server has no such page (is the address in sources.json RomM's?)";
    return "The server answered with status " + std::to_string(status);
}

class RommSource final : public Source
{
  public:
    RommSource(std::string url, std::string authorization, std::vector<std::string> platforms)
        : url_(std::move(url)), authorization_(std::move(authorization)),
          platforms_(std::move(platforms))
    {
    }

    std::string address() const override
    {
        return url_;
    }

    bool list(std::vector<SourceGame> *games, std::string *error, const Stopped &stopped,
              unsigned timeout) override
    {
        std::string filter;
        if (!platforms_.empty() && !platform_filter(stopped, timeout, &filter, error))
            return false;
        games->clear();
        /* From where the last page ended: a server may hand out fewer games a page than asked
         * for. */
        size_t offset = 0;
        for (int page = 0; page < most_pages; ++page)
        {
            std::string body;
            if (!get("/api/roms?with_files=true&limit=" + std::to_string(page_size) +
                         "&offset=" + std::to_string(offset) + filter +
                         "&order_by=name&order_dir=asc&with_char_index=false"
                         "&with_filter_values=false&with_rom_id_index=false",
                     stopped, timeout, &body, error))
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

    bool cover(const SourceGame &game, std::string *picture, const Stopped &stopped) override
    {
        if (game.cover.empty())
            return false;
        /* The server's own pictures may need the sign-in; another site's (IGDB) never gets it.
         * Its "?ts=..." only tells versions of the server's own picture apart. */
        const bool own =
            game.cover.rfind("http://", 0) != 0 && game.cover.rfind("https://", 0) != 0;
        ps5_scraper::Request request;
        request.url = own ? url_ + game.cover.substr(0, game.cover.find('?')) : game.cover;
        if (own && !authorization_.empty())
            request.headers.emplace_back("Authorization", authorization_);
        request.limit = most_cover;
        request.stopped = stopped;
        const ps5_scraper::Response response = client().send(request);
        if (response.status != 200 || !response.error.empty() || response.cancelled ||
            response.too_large)
            return false;
        *picture = response.body;
        return true;
    }

    bool fetch(const SourceGame &, const SourceFile &file, uint64_t offset, Receiver &receiver,
               std::string *error) override
    {
        /* The file by its own id, as RomM's feeds give it. */
        const std::string name = file.name.substr(file.name.find_last_of('/') + 1);
        for (;;)
        {
            ps5_scraper::Request request;
            request.url =
                url_ + "/api/roms/" + file.id + "/files/content/" + ps5_scraper::url_encode(name);
            if (!authorization_.empty())
                request.headers.emplace_back("Authorization", authorization_);
            if (offset > 0)
                request.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-");
            request.stopped = [&receiver] { return receiver.stopped(); };
            int status = 0;
            /* 206: from the offset asked for; 200: the whole file. Anything else is an error
             * page. */
            request.begin = [&](int code)
            {
                status = code;
                return (code == 200 || code == 206) && receiver.begin(code == 200);
            };
            request.sink = [&receiver](const char *data, size_t size)
            { return receiver.take(data, size); };
            const ps5_scraper::Response response = client().send(request);
            /* 416: the file on the server is shorter than what was begun: it starts again. */
            if (status == 416 && offset > 0 && !receiver.stopped())
            {
                offset = 0;
                continue;
            }
            if (status != 200 && status != 206)
            {
                *error = status == 0 ? (response.error.empty() ? "The server did not answer"
                                                               : response.error)
                                     : status_error(status);
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

  private:
    /* A JSON answer of the server to a GET of `path`, signed in; false with *error. */
    bool get(const std::string &path, const Stopped &stopped, unsigned timeout, std::string *body,
             std::string *error) const
    {
        ps5_scraper::Request request;
        request.url = url_ + path;
        if (!authorization_.empty())
            request.headers.emplace_back("Authorization", authorization_);
        request.limit = most_json;
        request.stopped = stopped;
        request.timeout = timeout;
        const ps5_scraper::Response response = client().send(request);
        if (response.cancelled)
            *error = "Stopped";
        else if (response.too_large)
            *error = "The server's answer is too large";
        else if (response.status == 0)
            *error = response.error.empty() ? "The server did not answer" : response.error;
        else if (response.status != 200)
            *error = status_error(response.status);
        else
        {
            *body = response.body;
            return true;
        }
        return false;
    }

    /* "&platform_ids=..." of the platforms the entry names, by their slugs on the server. */
    bool platform_filter(const Stopped &stopped, unsigned timeout, std::string *filter,
                         std::string *error) const
    {
        std::string body;
        if (!get("/api/platforms", stopped, timeout, &body, error))
            return false;
        Json platforms;
        if (!Json::parse(body, &platforms) || platforms.kind != Json::array)
        {
            *error = "The server's platform list cannot be read (is the address in sources.json "
                     "RomM's?)";
            return false;
        }
        for (const Json &item : platforms.items)
            for (const std::string &wanted : platforms_)
                if (lower(text(item, "slug")) == wanted || lower(text(item, "fs_slug")) == wanted)
                    *filter += "&platform_ids=" + id_text(item["id"]);
        if (filter->empty())
        {
            *error = "The server has none of the platforms sources.json names";
            return false;
        }
        return true;
    }

    const std::string url_;
    const std::string authorization_;
    const std::vector<std::string> platforms_;
};
} // namespace

std::unique_ptr<Source> make_source(const Json &settings, std::string *error)
{
    const std::string url = normal_url(text(settings, "url"));
    if (url.empty())
    {
        *error = "No server address (\"url\") in sources.json";
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
    return std::unique_ptr<Source>(new RommSource(url, authorization, platforms));
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
