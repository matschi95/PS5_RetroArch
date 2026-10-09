/* PS5 RetroArch - the backends, by their "type" in the config files (src/remote/backends.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "backends.h"

#include "romm/romm_saves.h"
#include "romm/romm_source.h"

namespace ps5::remote
{
const std::vector<Backend> &backends()
{
    static const std::vector<Backend> all = {
        {"romm", romm::make_source, romm::make_saves, &romm::pairing},
    };
    return all;
}

const Backend *find_backend(const std::string &type)
{
    for (const Backend &backend : backends())
        if (type == backend.type)
            return &backend;
    return nullptr;
}

std::unique_ptr<Source> make_source(const std::string &type, const Json &settings,
                                    std::string *error)
{
    const Backend *backend = find_backend(type);
    if (backend == nullptr)
    {
        *error = type.empty() ? "A source in sources.json has no \"type\""
                              : "Unknown source type \"" + type + "\"";
        return nullptr;
    }
    if (backend->source == nullptr)
    {
        *error = "A \"" + type + "\" server has no games to download";
        return nullptr;
    }
    return backend->source(settings, error);
}

std::unique_ptr<SaveStore> make_save_store(const std::string &type, const Json &settings,
                                           const std::string &folder, std::string *error)
{
    const Backend *backend = find_backend(type);
    if (backend == nullptr)
    {
        *error = type.empty() ? "save-sync.json names no server (\"type\")"
                              : "Unknown server type \"" + type + "\" in save-sync.json";
        return nullptr;
    }
    if (backend->saves == nullptr)
    {
        *error = "A \"" + type + "\" server cannot keep save data";
        return nullptr;
    }
    return backend->saves(settings, folder, error);
}
} // namespace ps5::remote
