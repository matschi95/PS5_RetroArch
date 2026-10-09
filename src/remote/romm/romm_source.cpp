/* PS5 RetroArch - the RomM backend of the download sources (src/remote/romm/romm_source.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_source.h"

#include "romm_client.h"

#include <utility>

namespace ps5::remote::romm
{
namespace
{
/* The most a cover may be. */
constexpr uint64_t most_cover = 8u << 20;

class RommSource final : public Source
{
  public:
    explicit RommSource(std::unique_ptr<Client> client) : client_(std::move(client))
    {
    }

    std::string address() const override
    {
        return client_->url();
    }

    bool list(std::vector<SourceGame> *games, std::string *error, const Stopped &stopped,
              unsigned timeout) override
    {
        return client_->games(games, stopped, error, timeout);
    }

    bool cover(const SourceGame &game, std::string *picture, const Stopped &stopped) override
    {
        if (game.cover.empty())
            return false;
        /* Its "?ts=..." only tells versions of the server's own picture apart. */
        const bool own =
            game.cover.rfind("http://", 0) != 0 && game.cover.rfind("https://", 0) != 0;
        return client_->fetch(own ? game.cover.substr(0, game.cover.find('?')) : game.cover,
                              most_cover, stopped, picture);
    }

    bool fetch(const SourceGame &, const SourceFile &file, uint64_t offset, Receiver &receiver,
               std::string *error) override
    {
        /* The file by its own id, as RomM's feeds give it. */
        const std::string name = file.name.substr(file.name.find_last_of('/') + 1);
        for (;;)
        {
            ps5_scraper::Request request;
            request.url = client_->url() + "/api/roms/" + file.id + "/files/content/" +
                          ps5_scraper::url_encode(name);
            if (!client_->authorization().empty())
                request.headers.emplace_back("Authorization", client_->authorization());
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
            const ps5_scraper::Response response = Client::http().send(request);
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
                                     : client_->status_error(status, "/api/roms");
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
    const std::unique_ptr<Client> client_;
};
} // namespace

std::unique_ptr<Source> make_source(const Json &settings, std::string *error)
{
    std::unique_ptr<Client> client = Client::make(settings, "sources.json", error);
    if (!client)
        return nullptr;
    return std::unique_ptr<Source>(new RommSource(std::move(client)));
}
} // namespace ps5::remote::romm
