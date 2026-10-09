/* PS5 RetroArch - the file operations the download sources need.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "files.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5::remote::files
{
bool read(const std::string &path, std::string *text, std::size_t limit)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    text->clear();
    char buffer[16384];
    std::size_t n;
    bool fits = true;
    while (fits && (n = std::fread(buffer, 1, sizeof buffer, file)) > 0)
    {
        fits = text->size() + n <= limit;
        if (fits)
            text->append(buffer, n);
    }
    const bool ok = fits && !std::ferror(file);
    std::fclose(file);
    return ok;
}

bool write(const std::string &path, const std::string &text)
{
    const std::string temporary = path + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = std::fflush(file) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    if (ok && std::rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    std::remove(temporary.c_str());
    return false;
}

bool make_folders(const std::string &path)
{
    for (std::size_t at = 1; at <= path.size(); at++)
        if (at == path.size() || path[at] == '/')
        {
            const std::string folder = path.substr(0, at);
            if (mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST)
                return false;
        }
    return is_folder(path);
}

void remove_tree(const std::string &path)
{
    struct stat status;
    if (lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode)) /* not a link's */
    {
        for (const auto &name : names(path))
            remove_tree(path + "/" + name);
        rmdir(path.c_str());
    }
    else
        std::remove(path.c_str());
}

std::vector<std::string> names(const std::string &folder)
{
    std::vector<std::string> found;
    DIR *dir = opendir(folder.c_str());
    if (!dir)
        return found;
    while (const struct dirent *entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name != "." && name != "..")
            found.push_back(name);
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    return found;
}

int64_t size(const std::string &path)
{
    struct stat status;
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
        return -1;
    return static_cast<int64_t>(status.st_size);
}

bool is_folder(const std::string &path)
{
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

std::string parent(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : slash == 0 ? "/" : path.substr(0, slash);
}

std::string base_name(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string safe_name(const std::string &name)
{
    std::string out;
    for (unsigned char c : name)
        if (c >= 32 && c != 127)
            out += c == '/' || c == '\\' || c == ':' ? '-' : static_cast<char>(c);
    const std::size_t first = out.find_first_not_of(" .");
    out = first == std::string::npos ? "" : out.substr(first);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.size() > 120)
    {
        std::size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80)
            cut--;
        out.resize(cut);
    }
    return out.empty() ? "game" : out;
}
} // namespace ps5::remote::files
