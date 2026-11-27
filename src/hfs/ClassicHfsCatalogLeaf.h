// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsCatalogKey.h"
#include "ClassicHfsNode.h"

static inline int ClassicHfsCatalogLeafOffset(const unsigned char *sector, const uint16_t *offsets,
                                             unsigned count, int slot, ClassicHfsCatalogKey *key)
{
    if (slot < 0 || (unsigned)slot >= count || !ClassicHfsValidOffsets(count, offsets))
        return 0;
    return ClassicHfsCatalogPayload(sector + offsets[slot], offsets[slot + 1] - offsets[slot], key);
}

static inline int ClassicHfsReadLeafEntry(unsigned char *sector, const uint16_t *offsets,
                                         unsigned count, int slot, ClassicHfsCatalogKey *key,
                                         ClassicHfsCatalogRecord *record)
{
    int payload = ClassicHfsCatalogLeafOffset(sector, offsets, count, slot, key);
    if (!payload)
        return 0;
    unsigned start = offsets[slot] + payload;
    return !record || ClassicHfsDecodeCatalogRecord(sector + start, offsets[slot + 1] - start, record);
}

static inline int ClassicHfsUpdateLeafEntry(unsigned char *sector, const uint16_t *offsets,
                                           unsigned count, int slot, const ClassicHfsCatalogRecord *record)
{
    int payload = ClassicHfsCatalogLeafOffset(sector, offsets, count, slot, NULL);
    if (!payload)
        return 0;
    unsigned start = offsets[slot] + payload;
    if (sector[start] != record->cdrType)
        return 0;
    return ClassicHfsEncodeCatalogRecord(sector + start, offsets[slot + 1] - start, record) != 0;
}
