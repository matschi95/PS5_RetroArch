/* PS5 RetroArch - the backends, by their "type" in the config files: what each kind of server
 * can be used for.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each use has an interface of its own (a download source: src/remote/source.h, its firmware:
 * src/remote/firmware.h, the console's save data: src/remote/save_store.h), and a
 * backend makes an object of it for an entry of its config file, or has none (nullptr) when
 * its server cannot be used that way. A server's own folder (src/remote/<type>/) has what its
 * uses share (src/remote/romm/romm_client.h).
 *
 * A new backend: a line in backends() (src/remote/backends.cpp) with what it makes. A new use:
 * a column of Backend, a make_... below, and nullptr in it for the backends that cannot do it.
 */
#pragma once

#include "firmware.h"
#include "pairing.h"
#include "save_store.h"
#include "source.h"

#include <memory>
#include <string>
#include <vector>

namespace ps5::remote
{
struct Backend
{
    const char *type; /* "romm" */
    /* A download source from its entry in sources.json; nullptr: it has no games to download. */
    std::unique_ptr<Source> (*source)(const Json &settings, std::string *error);
    /* The firmware of a download source, from the same entry; nullptr: it has none. */
    std::unique_ptr<FirmwareSource> (*firmware)(const Json &settings, std::string *error);
    /* The save store from its entry in save-sync.json, keeping what it needs between starts in
     * `folder`; nullptr: it cannot keep save data. */
    std::unique_ptr<SaveStore> (*saves)(const Json &settings, const std::string &folder,
                                        std::string *error);
    /* Its save store's entry signed in by pairing (pairing.h); nullptr: only by what is typed
     * into save-sync.json. */
    const Pairing *pairing;
};

/* All of them, in the order the menu offers them. */
const std::vector<Backend> &backends();
/* The backend of a type; nullptr when there is none. */
const Backend *find_backend(const std::string &type);

/* Makes the source of a "type" from its entry in sources.json. nullptr with *error when the
 * type is unknown, cannot be a download source or the entry is not usable. */
std::unique_ptr<Source> make_source(const std::string &type, const Json &settings,
                                    std::string *error);
/* The firmware of a source, from its entry in sources.json; nullptr (without an error) when
 * its backend has none. */
std::unique_ptr<FirmwareSource> make_firmware_source(const std::string &type, const Json &settings,
                                                     std::string *error);
/* The same for the save store, from the entry in save-sync.json. */
std::unique_ptr<SaveStore> make_save_store(const std::string &type, const Json &settings,
                                           const std::string &folder, std::string *error);
} // namespace ps5::remote
