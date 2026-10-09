/* PS5 RetroArch - a downloaded file checked while it streams by.
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A source says what it knows of a file's contents (source.h: RomM hashes every game file
 * it can, its CRC32 among them). The bytes are checked against that as they come, on their
 * way to the drive, so the file is not read again afterwards; bytes the stream did not bring
 * (a download that went on from where it was) are fed from the drive first
 * (src/remote/remote.cpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps5::remote
{
/* What a downloaded file's contents say about it. */
enum class Verified : uint8_t
{
    intact,
    damaged, /* its bytes are not what they should be: it is downloaded again */
    unknown, /* it cannot be checked (the source knows nothing of it, or bytes did not all
              * come by): taken as it is */
};

class StreamCheck
{
  public:
    /* crc32: what the source says of the file, as hex digits ("" when it says nothing). */
    explicit StreamCheck(const std::string &crc32);

    /* The file's bytes from `offset` on, in order. Bytes it had already are left out; a gap
     * (bytes it did not get) leaves the file unchecked. */
    void feed(uint64_t offset, const void *data, size_t size);
    /* The file comes again from its start (a server that does not go on where it was). */
    void restart();
    /* Where the next bytes are expected: what it has of the file so far. */
    uint64_t next() const
    {
        return next_;
    }
    /* After the file's last byte. */
    Verified result() const;

  private:
    uint32_t wanted_ = 0;
    bool known_ = false;
    uint32_t crc_ = 0;
    uint64_t next_ = 0;
    bool broken_ = false; /* bytes were missed */
};
} // namespace ps5::remote
