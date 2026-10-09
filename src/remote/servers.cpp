/* PS5 RetroArch - the game servers as the WebUI sets them up (src/remote/servers.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "servers.h"

#include "backends.h"
#include "files.h"
#include "save_config.h"

#include <utility>

namespace ps5::remote
{
namespace
{
std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

/* sources.json: none is an empty list; false when it is there but no such file. */
bool load_sources(const std::string &file, Json *document, std::string *error)
{
    std::string text;
    if (!files::read(file, &text, 1u << 20))
    {
        *document = Json::record();
        document->set("sources", Json::list());
        return true;
    }
    if (!Json::parse(text, document) || document->kind != Json::object ||
        ((*document)["sources"].kind != Json::array && document->has("sources")))
    {
        *error = "sources.json is not valid JSON: fix or delete it over FTP first";
        return false;
    }
    if (!document->has("sources"))
        document->set("sources", Json::list());
    return true;
}

bool store(const std::string &file, const Json &document, std::string *error)
{
    if (files::make_folders(files::parent(file)) && files::write(file, document.write(true)))
        return true;
    *error = "Cannot write " + file;
    return false;
}

/* An entry's flag, true unless it says false. */
bool flag(const Json &entry, const char *key)
{
    return entry[key].kind != Json::boolean || entry[key].yes();
}

/* "http://nas.local:3000/romm" -> "nas.local" */
std::string host_of(const std::string &url)
{
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    const size_t end = url.find_first_of(":/", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

Json *entry_named(Json &sources, const std::string &name)
{
    for (Json &entry : sources.fields["sources"].items)
        if (entry.kind == Json::object && entry["name"].str() == name)
            return &entry;
    return nullptr;
}

/* save-sync.json names this server (its address and kind). */
bool syncs_with(const SaveConfig &config, const std::string &type, const std::string &url)
{
    return config.readable && !config.type.empty() && lower(config.type) == lower(type) &&
           config.settings["url"].str() == url;
}
} // namespace

Json servers_json(const std::string &config)
{
    Json list = Json::list();
    Json sources;
    std::string error;
    const SaveConfig saves = read_save_config(config + "/save-sync.json");
    bool sync_listed = false;
    if (load_sources(config + "/sources.json", &sources, &error))
        for (const Json &entry : sources["sources"].items)
        {
            if (entry.kind != Json::object)
                continue;
            Json &item = list.push(Json::record());
            const std::string type = lower(entry["type"].str()), url = entry["url"].str();
            const bool syncs = syncs_with(saves, type, url);
            sync_listed = sync_listed || syncs;
            item.set("name", Json::of(entry["name"].str().empty() ? type : entry["name"].str()));
            item.set("type", Json::of(type));
            item.set("url", Json::of(url));
            item.set("user",
                     Json::of(!entry["__server_user"].str().empty() ? entry["__server_user"].str()
                                                                    : entry["username"].str()));
            item.set("games", Json::of(flag(entry, "games")));
            item.set("firmware", Json::of(flag(entry, "firmware")));
            item.set("saves", Json::of(syncs && saves.automatic));
            item.set("states", Json::of(syncs && saves.states));
            item.set("signed_in",
                     Json::of(!entry["token"].str().empty() || !entry["username"].str().empty()));
        }
    /* A save sync of its own (paired in the remote core, or written by hand). */
    if (!sync_listed && saves.readable && !saves.type.empty() &&
        !saves.settings["url"].str().empty())
    {
        Json &item = list.push(Json::record());
        const std::string url = saves.settings["url"].str();
        item.set("name", Json::of(host_of(url)));
        item.set("type", Json::of(lower(saves.type)));
        item.set("url", Json::of(url));
        item.set("user", Json::of(saves.settings["__server_user"].str()));
        item.set("games", Json::of(false));
        item.set("firmware", Json::of(false));
        item.set("saves", Json::of(saves.automatic));
        item.set("states", Json::of(saves.states));
        item.set("signed_in", Json::of(true));
        item.set("save_sync_only", Json::of(true));
    }
    return list;
}

bool probe_server(const std::string &type, const std::string &url, ServerProbe *probe,
                  std::string *error)
{
    const Backend *backend = find_backend(lower(type));
    if (backend == nullptr || backend->sign_in == nullptr)
    {
        *error = "A \"" + type + "\" server cannot be set up here";
        return false;
    }
    return backend->sign_in->probe(url, probe, error);
}

bool save_server(const std::string &config, const ServerSetup &setup, const std::string &previous,
                 std::string *error)
{
    const std::string type = lower(setup.type);
    const Backend *backend = find_backend(type);
    if (backend == nullptr || backend->sign_in == nullptr)
    {
        *error = "A \"" + setup.type + "\" server cannot be set up here";
        return false;
    }
    if (!setup.games && !setup.firmware && !setup.saves)
    {
        *error = "Choose at least one thing the server is for";
        return false;
    }
    ServerProbe probe;
    if (!backend->sign_in->probe(setup.url, &probe, error))
        return false;
    const std::string sources_file = config + "/sources.json";
    Json sources;
    if (!load_sources(sources_file, &sources, error))
        return false;
    const std::string name = !setup.name.empty() ? setup.name : host_of(probe.url);
    Json *before = previous.empty() ? nullptr : entry_named(sources, previous);
    if (!previous.empty() && before == nullptr)
    {
        *error = "There is no server named " + previous + " any more";
        return false;
    }
    if (name != previous && entry_named(sources, name) != nullptr)
    {
        *error = "A server named " + name + " is set up already";
        return false;
    }
    /* The sign-in: a new one, or the one it has for the same server. */
    Json signed_in = Json::record();
    std::string user;
    if (!setup.password.empty())
    {
        if (!backend->sign_in->sign_in(probe.url, setup.user, setup.password, config + "/save-sync",
                                       &signed_in, &user, error))
            return false;
    }
    else if (before && lower((*before)["type"].str()) == type &&
             (*before)["url"].str() == probe.url && !(*before)["token"].str().empty())
    {
        signed_in.set("token", (*before)["token"]);
        user = (*before)["__server_user"].str();
    }
    else
    {
        *error = "Sign in with your account's name and password";
        return false;
    }
    /* The entry: the player's own fields kept, its sign-in and uses set. */
    Json entry = before ? *before : Json::record();
    for (const char *key : {"username", "password", "token", "__server_user"})
        entry.erase(key);
    entry.set("type", Json::of(type));
    entry.set("name", Json::of(name));
    entry.set("url", Json::of(probe.url));
    for (const std::string &key : signed_in.order)
        if (key != "url")
            entry.set(key, signed_in[key]);
    if (!user.empty())
        entry.set("__server_user", Json::of(user));
    entry.set("games", Json::of(setup.games));
    entry.set("firmware", Json::of(setup.firmware));
    const std::string old_url = before ? (*before)["url"].str() : "";
    if (before)
        *before = entry;
    else
        sources.fields["sources"].push(entry);

    /* The save sync: this server's, or none of it when it was this one's. */
    const std::string saves_file = config + "/save-sync.json";
    if (!prepare_save_config(saves_file))
    {
        *error = "save-sync.json is not valid JSON: fix or delete it over FTP first";
        return false;
    }
    const SaveConfig saves = read_save_config(saves_file);
    if (!store(sources_file, sources, error))
        return false;
    Json fields = Json::record();
    if (setup.saves)
    {
        fields.set("type", Json::of(type));
        fields.set("url", Json::of(probe.url));
        for (const std::string &key : signed_in.order)
            if (key != "url")
                fields.set(key, signed_in[key]);
        if (!user.empty())
            fields.set("__server_user", Json::of(user));
        fields.set("auto", Json::of(true));
        fields.set("states", Json::of(setup.states));
    }
    else if (!syncs_with(saves, type, old_url) && !syncs_with(saves, type, probe.url))
        return true;
    if (!set_save_server(saves_file, fields))
    {
        *error = "save-sync.json could not be written";
        return false;
    }
    return true;
}

bool remove_server(const std::string &config, const std::string &name, std::string *error)
{
    const std::string sources_file = config + "/sources.json";
    Json sources;
    if (!load_sources(sources_file, &sources, error))
        return false;
    std::vector<Json> &items = sources.fields["sources"].items;
    std::string type, url;
    for (auto at = items.begin(); at != items.end(); ++at)
        if (at->kind == Json::object && (*at)["name"].str() == name)
        {
            type = lower((*at)["type"].str());
            url = (*at)["url"].str();
            items.erase(at);
            if (!store(sources_file, sources, error))
                return false;
            break;
        }
    const std::string saves_file = config + "/save-sync.json";
    const SaveConfig saves = read_save_config(saves_file);
    /* A save sync of its own is taken out by its own name (the address's host). */
    const bool saves_only =
        url.empty() && saves.readable && host_of(saves.settings["url"].str()) == name;
    if (url.empty() && !saves_only)
    {
        *error = "There is no server named " + name;
        return false;
    }
    if ((saves_only || syncs_with(saves, type, url)) &&
        !set_save_server(saves_file, Json::record()))
    {
        *error = "save-sync.json could not be written";
        return false;
    }
    return true;
}
} // namespace ps5::remote
