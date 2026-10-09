/* PS5 RetroArch - a RomM server (https://github.com/rommapp/romm) as each of its backends talks
 * to it: its address, the sign-in and the games it has.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The download source (romm_source.h) is one such backend; others (the save data) sign in with
 * their own entry and use the same. What an entry of a config file says about the server:
 *
 *   "url": "http://192.168.1.20:3000",
 *   "token": "rmm_...",                      a RomM client API token (Profile > Client tokens),
 *   "username": "me", "password": "secret",  or the account's name and password instead
 *   "platforms": ["snes", "psx"]             optional: only these platforms' slugs
 *
 * Every backend takes RomM minimum_version or newer (the oldest tools/check-romm.py checked
 * with); an older server is refused before anything else is asked, saying so.
 */
#pragma once

#include "../../scraper_http.h"
#include "../firmware.h"
#include "../source.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ps5::remote::romm
{
/* The oldest RomM the backends take. */
inline constexpr char minimum_version[] = "5.3.0";

class Client
{
  public:
    /* From an entry of `file` (the config file's name, for what goes wrong: "sources.json").
     * nullptr with *error when the entry is not usable. */
    static std::unique_ptr<Client> make(const Json &settings, const std::string &file,
                                        std::string *error);

    /* "http://nas:3000", without a / at the end. */
    const std::string &url() const
    {
        return url_;
    }
    /* The header value of the sign-in ("Bearer ...", "Basic ..."); empty without one. */
    const std::string &authorization() const
    {
        return authorization_;
    }

    /* A JSON answer of the server to a GET of `path` ("/api/..."), signed in; false with
     * *error. timeout: seconds for each answer (0: the HTTP client's). */
    bool get(const std::string &path, const Stopped &stopped, std::string *body, std::string *error,
             unsigned timeout = 0) const;
    /* A file of at most `limit` bytes: the server's own (a path, signed in) or another site's
     * (a whole address, not signed in). False when it did not come whole with status 200. */
    bool fetch(const std::string &path_or_url, uint64_t limit, const Stopped &stopped,
               std::string *body) const;
    /* An answer of the server, whatever its status. */
    struct Answer
    {
        int status = 0;
        std::string body;
    };
    /* A request of `method` ("POST", "PUT"; GET with "" or nullptr) to `path`, signed in,
     * with a JSON body (none when empty). False with *error when no answer came. */
    bool send(const char *method, const std::string &path, const std::string &json, Answer *answer,
              std::string *error, const Stopped &stopped = {}) const;
    /* The file `file` sent as the form field `field` (named `name`), signed in. */
    bool upload(const char *method, const std::string &path, const std::string &field,
                const std::string &file, const std::string &name, Answer *answer,
                std::string *error, const Stopped &stopped = {}) const;
    /* What `path` has, into the file `file` (written whole or not at all). */
    bool download(const std::string &path, const std::string &file, std::string *error,
                  const Stopped &stopped = {}) const;
    /* A file of the server (`path`, signed in) from `offset` on, into the receiver; a server
     * that cannot go on from there, or holds less than was begun, sends it from its start
     * (Receiver::begin). False with *error, or when the receiver stopped it. */
    bool stream(const std::string &path, uint64_t offset, Receiver &receiver,
                std::string *error) const;
    /* All the games of the platforms asked for (all of them without), with their files. */
    bool games(std::vector<SourceGame> *games, const Stopped &stopped, std::string *error,
               unsigned timeout = 0) const;

    /* Whether the server is minimum_version or newer, by its heartbeat (asked until it was).
     * False with *error when it does not answer or is older; too_old() then says its version. */
    bool check_version(const Stopped &stopped, std::string *error, unsigned timeout = 0) const;
    /* The server's version when the last check found it older than minimum_version; "" else. */
    std::string too_old() const;

    /* What an answer's status means, for the screen. path: what was asked for ("/api/..."), so
     * a refusal (403) can name the token's scope that is missing. */
    std::string status_error(int status, const std::string &path = {}) const;

    /* The HTTP client of the thread asking: each thread keeps its connections. */
    static ps5_scraper::Http &http();

  private:
    Client(std::string url, std::string authorization, std::vector<std::string> platforms,
           std::string file)
        : url_(std::move(url)), authorization_(std::move(authorization)),
          platforms_(std::move(platforms)), file_(std::move(file))
    {
    }
    /* "&platform_ids=..." of the platforms the entry names, by their slugs on the server. */
    bool platform_filter(const Stopped &stopped, unsigned timeout, std::string *filter,
                         std::string *error) const;

    const std::string url_;
    const std::string authorization_;
    const std::vector<std::string> platforms_;
    const std::string file_;
    mutable std::mutex version_lock_;
    mutable bool version_ok_ = false;
    mutable std::string too_old_;
};

/* ---- the parts, for tests ---- */
/* Whether a version RomM's heartbeat says ("5.3.0", "5.4.0-alpha.1") is minimum_version or
 * newer; a version that is no number ("development") is taken to be. */
bool new_enough(const std::string &version);
/* The firmware of /api/firmware's answer, each with its platform's names from /api/platforms'
 * (the platform's folder, slug, display name and name); false when they are no such lists. */
bool parse_firmware(const std::string &firmware, const std::string &platforms,
                    std::vector<FirmwareFile> *files);
/* The server's address as typed, made usable: "nas:3000/" is http://nas:3000; "" when it is
 * not http or https. */
std::string normal_url(std::string url);
/* Adds the games of a page of /api/roms to games; false when the answer is no such page.
 * listed: the entries the page had; total: the games the server has. */
bool parse_page(const std::string &text, std::vector<SourceGame> *games, size_t *listed = nullptr,
                size_t *total = nullptr);
} // namespace ps5::remote::romm
