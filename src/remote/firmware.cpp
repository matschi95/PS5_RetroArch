/* PS5 RetroArch - firmware from the download sources (src/remote/firmware.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "firmware.h"

#include "../ps5_library.h"
#include "files.h"
#include "stream_check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace ps5::remote
{
namespace
{
/* The most a firmware file may be (the largest a core reads are a few MAME BIOS sets). */
constexpr uint64_t most_firmware = 128u << 20;

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

std::string trimmed(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/* An info file's `key = "value"` lines. */
std::map<std::string, std::string> info_values(const std::string &text)
{
    std::map<std::string, std::string> values;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string value = trimmed(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        values[trimmed(line.substr(0, equals))] = value;
    }
    return values;
}

/* A file's platform as the source names its system: the first name src/ps5_library.c knows. */
std::string platform_of(const FirmwareFile &file)
{
    for (const std::string &system : file.systems)
        if (const char *platform = ps5_library_platform(system.c_str()))
            return platform;
    return "";
}

/* Takes a firmware file into memory, checked as it comes. */
class Collector final : public Receiver
{
  public:
    Collector(const std::string &crc32, const Stopped &stopped) : check_(crc32), stopped_(stopped)
    {
    }
    bool begin(bool) override
    {
        bytes_.clear();
        check_.restart();
        return true;
    }
    bool take(const void *data, size_t size) override
    {
        if (bytes_.size() + size > most_firmware)
            return false;
        check_.feed(bytes_.size(), data, size);
        bytes_.append(static_cast<const char *>(data), size);
        return true;
    }
    bool stopped() override
    {
        return stopped_ && stopped_();
    }
    const std::string &bytes() const
    {
        return bytes_;
    }
    Verified result() const
    {
        return check_.result();
    }

  private:
    StreamCheck check_;
    const Stopped &stopped_;
    std::string bytes_;
};

/* Fetches one file to `path`; false with *error. */
bool fetch_one(FirmwareSource &from, const FirmwareFile &file, const std::string &path,
               const Stopped &stopped, std::string *error)
{
    Collector collector(file.crc32, stopped);
    if (!from.fetch(file, collector, error))
    {
        if (error->empty())
            *error = collector.bytes().size() >= most_firmware ? "the file is too large"
                                                               : "the download stopped";
        return false;
    }
    if (file.size > 0 && collector.bytes().size() != file.size)
    {
        *error = "the file did not come whole";
        return false;
    }
    if (collector.result() == Verified::damaged)
    {
        *error = "the file came damaged";
        return false;
    }
    if (!files::make_folders(files::parent(path)) || !files::write(path, collector.bytes()))
    {
        *error = "cannot write " + path;
        return false;
    }
    return true;
}

std::string joined(const std::vector<std::string> &names)
{
    std::string text;
    for (const std::string &name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text;
}
} // namespace

bool read_core_firmware(const std::string &info_file, CoreFirmware *core)
{
    std::string text;
    if (!files::read(info_file, &text, 1u << 20))
        return false;
    const std::map<std::string, std::string> values = info_values(text);
    const auto value = [&values](const std::string &key)
    {
        const auto found = values.find(key);
        return found != values.end() ? found->second : std::string();
    };
    *core = CoreFirmware();
    core->core = value("corename");
    /* Its databases, '|'-separated: "Sony - PlayStation|Sony - PlayStation (Unlicensed)". */
    const std::string databases = value("database");
    for (size_t at = 0; at <= databases.size();)
    {
        size_t end = databases.find('|', at);
        if (end == std::string::npos)
            end = databases.size();
        const std::string name = trimmed(databases.substr(at, end - at));
        if (const char *platform = name.empty() ? nullptr : ps5_library_platform(name.c_str()))
            if (std::find(core->platforms.begin(), core->platforms.end(), platform) ==
                core->platforms.end())
                core->platforms.push_back(platform);
        at = end + 1;
    }
    const long count = std::strtol(value("firmware_count").c_str(), nullptr, 10);
    for (long i = 0; i < count && i < 256; i++)
    {
        const std::string prefix = "firmware" + std::to_string(i);
        FirmwareNeed need;
        need.path = value(prefix + "_path");
        need.optional = lower(value(prefix + "_opt")) == "true";
        /* A path inside the system folder only. */
        if (need.path.empty() || need.path.front() == '/' ||
            need.path.find("..") != std::string::npos)
            continue;
        core->files.push_back(std::move(need));
    }
    return true;
}

std::vector<CoreFirmware> platform_firmware(const std::string &info, const std::string &cores,
                                            const std::string &platform)
{
    std::vector<CoreFirmware> found;
    for (const std::string &name : files::names(info))
    {
        const size_t dot = name.rfind(".info");
        if (dot == std::string::npos || dot + 5 != name.size())
            continue;
        CoreFirmware core;
        if (files::size(cores + "/" + name.substr(0, dot) + ".so") < 0 ||
            !read_core_firmware(info + "/" + name, &core) || core.files.empty())
            continue;
        if (std::find(core.platforms.begin(), core.platforms.end(), platform) !=
            core.platforms.end())
            found.push_back(std::move(core));
    }
    return found;
}

const FirmwareFile *pick_firmware(const std::vector<FirmwareFile> &files, const FirmwareNeed &need,
                                  const std::vector<std::string> &platforms)
{
    const std::string wanted = lower(files::base_name(need.path));
    const FirmwareFile *best = nullptr;
    int best_rank = -1;
    for (const FirmwareFile &file : files)
    {
        if (file.id.empty() || lower(file.name) != wanted)
            continue;
        const std::string platform = platform_of(file);
        const bool ours = !platform.empty() && std::find(platforms.begin(), platforms.end(),
                                                         platform) != platforms.end();
        /* Another platform's file of the same name is not this core's. */
        if (!platform.empty() && !ours && !platforms.empty())
            continue;
        const int rank = (ours ? 2 : 0) + (file.verified ? 1 : 0);
        if (rank > best_rank)
        {
            best = &file;
            best_rank = rank;
        }
    }
    return best;
}

Json firmware_json(const std::vector<FirmwareFile> &files)
{
    Json list = Json::list();
    for (const FirmwareFile &file : files)
    {
        Json &item = list.push(Json::record());
        item.set("id", Json::of(file.id));
        item.set("name", Json::of(file.name));
        Json &systems = item.set("systems", Json::list());
        for (const std::string &system : file.systems)
            systems.push(Json::of(system));
        item.set("size", Json::of(double(file.size)));
        item.set("crc32", Json::of(file.crc32));
        item.set("md5", Json::of(file.md5));
        item.set("sha1", Json::of(file.sha1));
        item.set("verified", Json::of(file.verified));
    }
    return list;
}

std::vector<FirmwareFile> firmware_files(const Json &json)
{
    std::vector<FirmwareFile> files;
    for (const Json &item : json.items)
    {
        FirmwareFile file;
        file.id = item["id"].str();
        file.name = item["name"].str();
        for (const Json &system : item["systems"].items)
            file.systems.push_back(system.str());
        file.size = item["size"].whole();
        file.crc32 = item["crc32"].str();
        file.md5 = item["md5"].str();
        file.sha1 = item["sha1"].str();
        file.verified = item["verified"].yes();
        if (!file.id.empty() && !file.name.empty())
            files.push_back(std::move(file));
    }
    return files;
}

FirmwareOutcome fetch_firmware(const CoreFirmware &core, const std::string &folder,
                               const std::vector<FirmwareOffer> &offers, const Stopped &stopped)
{
    FirmwareOutcome outcome;
    /* No system folder (RetroArch's setting empty: the content's folder then): none to fill. */
    if (folder.empty())
        return outcome;
    for (const FirmwareNeed &need : core.files)
    {
        const std::string path = folder + "/" + need.path;
        if (files::size(path) >= 0 || (stopped && stopped()))
            continue;
        const std::string name = files::base_name(need.path);
        bool found = false;
        std::string why;
        for (const FirmwareOffer &offer : offers)
        {
            const FirmwareFile *file =
                offer.from ? pick_firmware(offer.files, need, core.platforms) : nullptr;
            if (file == nullptr)
                continue;
            found = true;
            std::string error;
            if (fetch_one(*offer.from, *file, path, stopped, &error))
            {
                outcome.fetched.push_back(need.path);
                outcome.sources.push_back(offer.source);
                why.clear();
                break;
            }
            why = name + ": " + error;
            std::fprintf(stderr, "[firmware] %s from %s: %s\n", need.path.c_str(),
                         offer.source.c_str(), error.c_str());
        }
        if (!why.empty())
            outcome.failed.push_back(why);
        else if (!found && !need.optional)
            outcome.missing.push_back(name);
    }
    return outcome;
}

std::string firmware_notice(const FirmwareOutcome &outcome, bool any_offer)
{
    std::string text;
    if (!outcome.fetched.empty())
    {
        /* The source of the first; they mostly come from one. */
        text = "BIOS downloaded from " + outcome.sources.front() + ": " + joined(outcome.fetched);
    }
    if (!outcome.failed.empty())
        text += (text.empty() ? "" : ". ") + std::string("BIOS not downloaded: ") +
                joined(outcome.failed);
    /* A missing file is only news when a source could have had it. */
    if (any_offer && !outcome.missing.empty())
        text += (text.empty() ? "" : ". ") + std::string("BIOS missing, no source has it: ") +
                joined(outcome.missing);
    return text;
}
} // namespace ps5::remote
