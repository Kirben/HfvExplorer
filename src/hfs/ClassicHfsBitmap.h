// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

static inline unsigned ClassicHfsBitmapSectors(uint32_t blocks)
{
    return blocks / 4096 + (blocks % 4096 != 0);
}

// The boot blocks and master directory block occupy sectors 0 through 2.
static inline int ClassicHfsBitmapFits(unsigned start, unsigned sectors,
                                       unsigned allocationStart, uint32_t volumeSectors)
{
    return start >= 3 && sectors && start <= allocationStart &&
           sectors <= allocationStart - start && start <= volumeSectors &&
           sectors <= volumeSectors - start;
}

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

// Node zero holds the B-tree header. During tree growth the bitmap can
// temporarily describe only a prefix while additional map nodes are allocated.
static inline uint32_t ClassicHfsClaimTreeNode(void *map, uint32_t mapBytes,
                                               uint32_t nodes, uint32_t freeNodes)
{
    if (!map || !mapBytes || !freeNodes || freeNodes >= nodes || !ClassicHfsBitIsSet(map, 0))
        return 0;
    for (uint32_t i = 1; i < nodes && i / 8 < mapBytes; ++i) {
        if (!ClassicHfsBitIsSet(map, i)) {
            ClassicHfsSetBit(map, i);
            return i;
        }
    }
    return 0;
}

static inline int ClassicHfsCanReleaseTreeNode(const void *map, uint32_t mapBytes, uint32_t nodes,
                                               uint32_t freeNodes, uint32_t index)
{
    return map && index && index < nodes && index / 8 < mapBytes && freeNodes < nodes - 1 &&
           ClassicHfsBitIsSet(map, 0) && ClassicHfsBitIsSet(map, index);
}

static inline int ClassicHfsReleaseTreeNode(void *map, uint32_t mapBytes, uint32_t nodes,
                                            uint32_t freeNodes, uint32_t index)
{
    if (!ClassicHfsCanReleaseTreeNode(map, mapBytes, nodes, freeNodes, index))
        return 0;
    ClassicHfsClearBit(map, index);
    return 1;
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

// Publish the extent and accounting only after validating the allocation.
static inline unsigned ClassicHfsAllocateBlocks(void *bitmap, uint16_t count,
                                               uint16_t *hint, uint16_t *freeBlocks,
                                               uint16_t request, uint16_t *first)
{
    unsigned start = 0;
    if (*freeBlocks > count || !*freeBlocks)
        return 0;
    unsigned length = ClassicHfsFindFreeRun(bitmap, count, *hint, request, &start);
    if (!length || length > *freeBlocks)
        return 0;
    for (unsigned i = start; i < start + length; ++i)
        ClassicHfsSetBit(bitmap, i);
    *first = (uint16_t)start;
    *hint = start + length == count ? 0 : (uint16_t)(start + length);
    *freeBlocks -= (uint16_t)length;
    return length;
}
