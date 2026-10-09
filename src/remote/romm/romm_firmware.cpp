/* PS5 RetroArch - the RomM backend of the download sources' firmware
 * (src/remote/romm/romm_firmware.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_firmware.h"

#include "romm_client.h"

#include <utility>

namespace ps5::remote::romm
{
namespace
{
class RommFirmware final : public FirmwareSource
{
  public:
    explicit RommFirmware(std::unique_ptr<Client> client) : client_(std::move(client))
    {
    }

    bool list(std::vector<FirmwareFile> *files, std::string *error, const Stopped &stopped,
              unsigned timeout) override
    {
        /* With the platforms: a file names only its platform's id. */
        std::string platforms, firmware;
        if (!client_->get("/api/firmware", stopped, &firmware, error, timeout) ||
            !client_->get("/api/platforms", stopped, &platforms, error, timeout))
            return false;
        if (!parse_firmware(firmware, platforms, files))
        {
            *error = "The server's firmware list cannot be read";
            return false;
        }
        return true;
    }

    bool fetch(const FirmwareFile &file, Receiver &receiver, std::string *error) override
    {
        return client_->stream("/api/firmware/" + file.id + "/content/" +
                                   ps5_scraper::url_encode(file.name),
                               0, receiver, error);
    }

  private:
    const std::unique_ptr<Client> client_;
};
} // namespace

std::unique_ptr<FirmwareSource> make_firmware(const Json &settings, std::string *error)
{
    std::unique_ptr<Client> client = Client::make(settings, "sources.json", error);
    if (!client)
        return nullptr;
    return std::unique_ptr<FirmwareSource>(new RommFirmware(std::move(client)));
}
} // namespace ps5::remote::romm
