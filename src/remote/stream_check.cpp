/* PS5 RetroArch - a downloaded file checked while it streams by (src/remote/stream_check.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "stream_check.h"

#include "remote.h"

#include <cstdlib>
#include <zlib.h>

namespace ps5::remote
{
StreamCheck::StreamCheck(const std::string &crc32)
{
    const std::string digits = normal_crc(crc32);
    known_ = !digits.empty();
    wanted_ = known_ ? uint32_t(std::strtoul(digits.c_str(), nullptr, 16)) : 0;
    crc_ = uint32_t(::crc32(0, nullptr, 0));
}

void StreamCheck::feed(uint64_t offset, const void *data, size_t size)
{
    if (offset > next_)
    {
        broken_ = true; /* a gap */
        return;
    }
    /* The part it had already is left out. */
    const uint64_t skip = next_ - offset;
    if (skip >= size)
        return;
    const unsigned char *bytes = static_cast<const unsigned char *>(data) + skip;
    size_t left = size_t(size - skip);
    while (left > 0)
    {
        /* zlib takes at most an unsigned int's worth at once. */
        const unsigned piece = unsigned(left > (1u << 30) ? (1u << 30) : left);
        crc_ = uint32_t(::crc32(crc_, bytes, piece));
        bytes += piece;
        left -= piece;
    }
    next_ = offset + size;
}

void StreamCheck::restart()
{
    crc_ = uint32_t(::crc32(0, nullptr, 0));
    next_ = 0;
    broken_ = false;
}

Verified StreamCheck::result() const
{
    if (!known_ || broken_)
        return Verified::unknown;
    return crc_ == wanted_ ? Verified::intact : Verified::damaged;
}
} // namespace ps5::remote
