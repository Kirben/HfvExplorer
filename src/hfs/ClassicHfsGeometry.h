// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

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
