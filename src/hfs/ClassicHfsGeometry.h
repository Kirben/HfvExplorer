// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include "ClassicHfsBitmap.h"

typedef struct {
    uint32_t sectorsPerBlock;
    uint32_t blockBytes;
    uint16_t firstAllocationSector;
    uint16_t allocationBlocks;
    uint32_t treeClumpBytes;
} ClassicHfsFormatGeometry;

static inline int ClassicHfsPlanFormat(uint32_t sectors, ClassicHfsFormatGeometry *plan)
{
    if (sectors < 1600) // minimum supported format is 800 KiB
        return 0;
    uint32_t perBlock = 1 + sectors / 65536;
    uint32_t bitmap = ClassicHfsBitmapSectors(sectors / perBlock);
    uint32_t first = 3 + bitmap;
    uint32_t blocks = (sectors - first - 2) / perBlock;
    uint32_t bytes = perBlock * 512;
    // The master directory stores clump sizes in unsigned 32-bit bytes.
    uint32_t clumpBlocks = blocks / 128;
    if (clumpBlocks > UINT32_MAX / bytes)
        clumpBlocks = UINT32_MAX / bytes;
    plan->sectorsPerBlock = perBlock;
    plan->blockBytes = bytes;
    plan->firstAllocationSector = (uint16_t)first;
    plan->allocationBlocks = (uint16_t)blocks;
    plan->treeClumpBytes = clumpBlocks * bytes;
    return 1;
}

enum { ClassicHfsBadForkSize = -1, ClassicHfsForkSizeLimit = 0, ClassicHfsForkSizeOk = 1 };

static inline int ClassicHfsGrowthRequest(uint32_t physical, uint32_t blockBytes,
                                          uint32_t clumpBytes, uint16_t *blocks)
{
    if (!blockBytes || physical % blockBytes)
        return ClassicHfsBadForkSize;
    uint32_t remaining = (UINT32_MAX - physical) / blockBytes;
    if (!remaining)
        return ClassicHfsForkSizeLimit;
    uint32_t request = clumpBytes / blockBytes;
    if (!request)
        request = 1;
    if (request > remaining)
        request = remaining;
    *blocks = (uint16_t)(request > UINT16_MAX ? UINT16_MAX : request);
    return ClassicHfsForkSizeOk;
}

static inline int ClassicHfsTruncatedSize(uint32_t logical, uint32_t physical,
                                          uint32_t blockBytes, uint32_t *rounded)
{
    if (!blockBytes || physical % blockBytes)
        return ClassicHfsBadForkSize;
    uint64_t blocks = ((uint64_t)logical + blockBytes - 1) / blockBytes;
    uint64_t bytes = blocks * blockBytes;
    if (bytes > UINT32_MAX)
        return ClassicHfsForkSizeLimit;
    if (bytes > physical)
        return ClassicHfsBadForkSize;
    *rounded = (uint32_t)bytes;
    return ClassicHfsForkSizeOk;
}

// Classic volume lengths and logical sector numbers are 32-bit. Byte offsets
// into an image may be larger. Keep intermediate sector arithmetic wide.
static inline int ClassicHfsVolumeSectors(uint16_t first, uint16_t blocks,
                                          uint32_t sectorsPerBlock, uint32_t *size)
{
    uint64_t sectors = first + (uint64_t)blocks * sectorsPerBlock + 2;
    if (!sectorsPerBlock || sectors > UINT32_MAX)
        return 0;
    *size = (uint32_t)sectors;
    return 1;
}

static inline int ClassicHfsAllocationSector(uint16_t first, uint16_t blocks,
                                             uint32_t sectorsPerBlock, uint32_t volumeSize,
                                             unsigned blockNumber, unsigned index,
                                             uint32_t *sector)
{
    if (blockNumber >= blocks || index >= sectorsPerBlock)
        return 0;
    uint64_t address = first + (uint64_t)blockNumber * sectorsPerBlock + index;
    if (address >= volumeSize)
        return 0;
    *sector = (uint32_t)address;
    return 1;
}
