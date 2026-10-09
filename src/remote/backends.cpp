/* PS5 RetroArch - the backends of the download sources: a source for each "type" of
 * sources.json (src/remote/source.h). The one place that knows them.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "source.h"

namespace ps5::remote
{
std::unique_ptr<Source> make_source(const std::string &type, const Json &settings,
                                    std::string *error)
{
    (void)settings;
    *error = type.empty() ? "A source in sources.json has no \"type\""
                          : "Unknown source type \"" + type + "\"";
    return nullptr;
}
} // namespace ps5::remote
