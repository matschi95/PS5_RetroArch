/* PS5 RetroArch - firmware (BIOS files) from the download sources: those a core's info names,
 * fetched when the core lacks them.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A core's info file lists the firmware it reads (firmwareN_path, in RetroArch's system
 * folder, firmwareN_opt when it runs without). A source whose backend has firmware
 * (backends.h) lists its own with its games (remote.h keeps that list beside its game list,
 * so a start needs no network to tell what is there); what a core lacks is fetched from the
 * first source that has a file of that name, checked against its CRC32 and written whole.
 * Only what an installed core names is fetched, nothing else a server has. A file on the
 * console is never replaced.
 *
 * A new backend: a class deriving from FirmwareSource, made by its line of backends().
 */
#pragma once

#include "source.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps5::remote
{
struct FirmwareFile
{
    std::string id;   /* the source's own name for it */
    std::string name; /* its file name ("scph5501.bin") */
    /* What the source calls the system it is for, most precise first, as SourceGame's. */
    std::vector<std::string> systems;
    uint64_t size = 0;
    std::string crc32, md5, sha1; /* hex digits; empty when the source does not know */
    bool verified = false;        /* the source matched it to a known good dump */
};

class FirmwareSource
{
  public:
    virtual ~FirmwareSource() = default;
    /* All of its firmware. Blocking (the network); false with *error, in English. */
    virtual bool list(std::vector<FirmwareFile> *files, std::string *error, const Stopped &stopped,
                      unsigned timeout = 0) = 0;
    /* A file whole, into the receiver. True when all of it came. */
    virtual bool fetch(const FirmwareFile &file, Receiver &receiver, std::string *error) = 0;
};

/* A file a core reads. */
struct FirmwareNeed
{
    std::string path; /* in the system folder ("scph5501.bin", "dc/dc_boot.bin") */
    bool optional = false;
};

/* What a core's info says of its firmware. */
struct CoreFirmware
{
    std::string core; /* its corename ("Beetle PSX HW"), which its system folder may depend on */
    /* The platforms it runs (src/ps5_library.h's ids, of its info's databases). */
    std::vector<std::string> platforms;
    std::vector<FirmwareNeed> files;
};

/* A core's info file; false when it cannot be read. */
bool read_core_firmware(const std::string &info_file, CoreFirmware *core);
/* The installed cores (an info in `info` with its core in `cores`) that run a platform. */
std::vector<CoreFirmware> platform_firmware(const std::string &info, const std::string &cores,
                                            const std::string &platform);
/* The source's file for what a core reads: one of the same file name (in any case), not of
 * another platform than the core's; of one of its platforms first, then one the source
 * verified. nullptr when it has none. */
const FirmwareFile *pick_firmware(const std::vector<FirmwareFile> &files, const FirmwareNeed &need,
                                  const std::vector<std::string> &platforms);

/* A source's firmware list as remote.h keeps it, and back. */
Json firmware_json(const std::vector<FirmwareFile> &files);
std::vector<FirmwareFile> firmware_files(const Json &json);

/* A source's firmware, as its last list said. */
struct FirmwareOffer
{
    std::string source; /* its name, as the menu shows it */
    std::shared_ptr<FirmwareSource> from;
    std::vector<FirmwareFile> files;
};

struct FirmwareOutcome
{
    std::vector<std::string> fetched; /* the files written ("scph5501.bin") */
    std::vector<std::string> sources; /* where each came from (its source's name) */
    std::vector<std::string> failed;  /* "scph5501.bin: why", one each that did not come */
    std::vector<std::string> missing; /* those the core needs (not optional) that no source has */
};

/* Fetches what a core lacks in `folder` (its system folder; none when empty) from the offers,
 * in their order. Without a lack it asks nothing. Blocking. */
FirmwareOutcome fetch_firmware(const CoreFirmware &core, const std::string &folder,
                               const std::vector<FirmwareOffer> &offers, const Stopped &stopped);
/* What the player is told of it; empty when nothing happened worth telling (nothing
 * lacked, or no source can have firmware). */
std::string firmware_notice(const FirmwareOutcome &outcome, bool any_offer);
} // namespace ps5::remote
