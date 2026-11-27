// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"
#include <string.h>

// Sequential fields within classic HFS catalog, extent, B-tree and volume records.
static inline unsigned char ClassicHfsTake8(unsigned char **cursor)
{
    return *(*cursor)++;
}

static inline uint16_t ClassicHfsTake16(unsigned char **cursor)
{
    uint16_t value = ClassicHfsRead16(*cursor);
    *cursor += 2;
    return value;
}

static inline uint32_t ClassicHfsTake32(unsigned char **cursor)
{
    uint32_t value = ClassicHfsRead32(*cursor);
    *cursor += 4;
    return value;
}

static inline void ClassicHfsAppend8(unsigned char **cursor, unsigned char value)
{
    *(*cursor)++ = value;
}

static inline void ClassicHfsAppend16(unsigned char **cursor, uint16_t value)
{
    ClassicHfsWrite16(*cursor, value);
    *cursor += 2;
}

static inline void ClassicHfsAppend32(unsigned char **cursor, uint32_t value)
{
    ClassicHfsWrite32(*cursor, value);
    *cursor += 4;
}

// Width includes the length byte. Classic HFS fixed name fields use at most
// 31 characters. Preserve the legacy empty-name result for an invalid length.
static inline void ClassicHfsTakeName(unsigned char **cursor, char *name, size_t width)
{
    size_t length = **cursor;
    if (length >= width)
        length = 0;
    memcpy(name, *cursor + 1, length);
    name[length] = 0;
    *cursor += width;
}

static inline void ClassicHfsAppendName(unsigned char **cursor, const char *name, size_t width)
{
    size_t length = 0;
    while (length < width && name[length])
        ++length;
    if (length == width)
        length = 0;
    memset(*cursor, 0, width);
    **cursor = (unsigned char)length;
    memcpy(*cursor + 1, name, length);
    *cursor += width;
}
