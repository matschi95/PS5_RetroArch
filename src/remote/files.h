/* PS5 RetroArch - the file operations the download sources need.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5::remote::files
{
/* A whole file, up to limit bytes: false when it cannot be read or is larger. */
bool read(const std::string &path, std::string *text, std::size_t limit = 64u << 20);
/* Writes a file whole: to a temporary file beside it, renamed, so a reader sees the old
 * file or the new one. */
bool write(const std::string &path, const std::string &text);
/* Creates a folder and the folders above it. */
bool make_folders(const std::string &path);
/* Removes a file, or a folder with everything in it. */
void remove_tree(const std::string &path);
/* The names in a folder, sorted, without "." and ".."; none when it cannot be read. */
std::vector<std::string> names(const std::string &folder);
/* A file's size, or -1 when it is not a file. */
int64_t size(const std::string &path);
bool is_folder(const std::string &path);
/* "/a/b/c.txt" -> "/a/b" */
std::string parent(const std::string &path);
/* "/a/b/c.txt" -> "c.txt" */
std::string base_name(const std::string &path);
/* A name usable as one file or folder name: no '/', no control characters, not "." or
 * "..", trimmed, at most 120 bytes (cut at a whole character); "game" when nothing is
 * left. */
std::string safe_name(const std::string &name);
} // namespace ps5::remote::files
