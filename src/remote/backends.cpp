/* PS5 RetroArch - the backends, by their "type" in the config files (src/remote/backends.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "backends.h"

#include "romm/romm_source.h"

namespace ps5::remote
{
const std::vector<Backend> &backends()
{
    static const std::vector<Backend> all = {
        {"romm", romm::make_source},
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
} // namespace ps5::remote
