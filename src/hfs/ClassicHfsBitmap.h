// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

static inline int ClassicHfsBitIsSet(const void *bitmap, uint32_t index)
{
    const unsigned char *bytes = (const unsigned char *)bitmap;
    return (bytes[index / 8] & (0x80u >> (index % 8))) != 0;
}

static inline void ClassicHfsSetBit(void *bitmap, uint32_t index)
{
    unsigned char *bytes = (unsigned char *)bitmap;
    bytes[index / 8] |= (unsigned char)(0x80u >> (index % 8));
}

static inline void ClassicHfsClearBit(void *bitmap, uint32_t index)
{
    unsigned char *bytes = (unsigned char *)bitmap;
    bytes[index / 8] &= (unsigned char)~(0x80u >> (index % 8));
}

// Validate the complete range before freeing any of it. Keep the arithmetic
// wide enough to reject extents whose end exceeds the 16-bit volume limit.
static inline int ClassicHfsReleaseBlocks(void *bitmap, unsigned count, unsigned first,
                                          unsigned length, uint16_t *freeBlocks)
{
    if (!bitmap || !length || first >= count || length > count - first ||
        count > UINT16_MAX || *freeBlocks > count - length)
        return 0;
    for (unsigned i = first; i < first + length; ++i)
        if (!ClassicHfsBitIsSet(bitmap, i))
            return 0;
    for (unsigned i = first; i < first + length; ++i)
        ClassicHfsClearBit(bitmap, i);
    *freeBlocks = (uint16_t)(*freeBlocks + length);
    return 1;
}

// Prefer the first adequate extent from the allocation hint. Otherwise use
// the longest partial extent. A run can cross the hint, never the disk end.
static inline unsigned ClassicHfsFindFreeRun(const void *bitmap, unsigned count,
                                             unsigned hint, unsigned request, unsigned *first)
{
    if (!bitmap || !request || !count || hint > count)
        return 0;
    unsigned best = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned pos = pass ? 0 : hint;
        unsigned stop = pass ? hint : count;
        while (pos < stop) {
            if (ClassicHfsBitIsSet(bitmap, pos)) {
                ++pos;
                continue;
            }
            unsigned start = pos;
            do {
                ++pos;
            } while (pos < count && pos - start < request && !ClassicHfsBitIsSet(bitmap, pos));
            if (pos - start > best) {
                best = pos - start;
                *first = start;
                if (best == request)
                    return best;
            }
        }
    }
    return best;
}
