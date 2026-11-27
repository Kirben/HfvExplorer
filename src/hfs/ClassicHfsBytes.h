// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

// Classic HFS disk fields can be unaligned. Shift unsigned values so that
// decoding a high-bit object ID or fork length never overflows a signed int.
static inline uint16_t ClassicHfsRead16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static inline uint32_t ClassicHfsRead32(const unsigned char *bytes)
{
    return (uint32_t)ClassicHfsRead16(bytes) << 16 | ClassicHfsRead16(bytes + 2);
}

static inline void ClassicHfsWrite16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)value;
}

static inline void ClassicHfsWrite32(unsigned char *bytes, uint32_t value)
{
    ClassicHfsWrite16(bytes, (uint16_t)(value >> 16));
    ClassicHfsWrite16(bytes + 2, (uint16_t)value);
}
