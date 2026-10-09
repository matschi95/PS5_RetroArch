/* PS5 RetroArch - the save sync's settings (src/remote/save_config.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "save_config.h"

#include "files.h"

namespace ps5::remote
{
namespace
{
/* The file as JSON: *exists false when there is none; false when it is no JSON object. */
bool load(const std::string &file, bool *exists, Json *document)
{
    std::string content;
    *exists = files::read(file, &content);
    if (!*exists)
    {
        *document = Json::record();
        return true;
    }
    return Json::parse(content, document) && document->kind == Json::object;
}

bool store(const std::string &file, const Json &document)
{
    return files::make_folders(files::parent(file)) && files::write(file, document.write(true));
}
} // namespace

SaveConfig read_save_config(const std::string &file)
{
    SaveConfig config;
    bool exists = false;
    Json document;
    if (!load(file, &exists, &document))
    {
        config.error = "save-sync.json is no JSON object";
        return config;
    }
    config.readable = true;
    if (document["auto"].kind == Json::boolean)
        config.automatic = document["auto"].flag;
    if (document["states"].kind == Json::boolean)
        config.states = document["states"].flag;
    if (document["type"].kind == Json::string)
        config.type = document["type"].text;
    config.settings = document;
    return config;
}

bool prepare_save_config(const std::string &file)
{
    bool exists = false;
    Json document;
    if (!load(file, &exists, &document))
        return false;
    const Json before = document;
    if (!document.has("version"))
        document.set("version", Json::of(1.0));
    if (!document.has("auto"))
        document.set("auto", Json::of(true));
    if (!document.has("states"))
        document.set("states", Json::of(true));
    for (const char *key : {"type", "url"})
        if (!document.has(key))
            document.set(key, Json::of(""));
    if (!document.has("token") && !document.has("username"))
        document.set("token", Json::of(""));
    if (exists && document == before)
        return true;
    return store(file, document);
}

bool set_save_server(const std::string &file, const Json &fields)
{
    bool exists = false;
    Json document;
    if (!load(file, &exists, &document))
        return false;
    for (const char *key : {"token", "username", "password", "__server_user"})
        document.erase(key);
    if (fields.order.empty())
        for (const char *key : {"type", "url", "token"})
            document.set(key, Json::of(""));
    for (const std::string &key : fields.order)
        document.set(key, fields[key]);
    return store(file, document);
}
} // namespace ps5::remote
