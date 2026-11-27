// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdlib.h>

enum { ClassicHfsCacheSlots = 128, ClassicHfsCacheValid = 1, ClassicHfsCacheDirty = 2 };

typedef struct ClassicHfsCacheEntry {
    int flags;
    unsigned count;
    unsigned long bnum;
    unsigned char (*data)[512];
    struct ClassicHfsCacheEntry *cnext, *cprev, *hnext, *hprev;
} ClassicHfsCacheEntry;

typedef int (*ClassicHfsWriteCachedSector)(void *, unsigned long, unsigned char (*)[512]);

static inline int ClassicHfsWriteBackEntry(ClassicHfsCacheEntry *entry,
                                           ClassicHfsWriteCachedSector store, void *context)
{
    if ((entry->flags & (ClassicHfsCacheValid | ClassicHfsCacheDirty)) !=
        (ClassicHfsCacheValid | ClassicHfsCacheDirty))
        return 0;
    if (store(context, entry->bnum, entry->data) < 0)
        return -1;
    entry->flags &= ~ClassicHfsCacheDirty;
    return 0;
}

static inline int ClassicHfsCompareCacheAddress(const void *left, const void *right)
{
    unsigned long a = (*(ClassicHfsCacheEntry *const *)left)->bnum;
    unsigned long b = (*(ClassicHfsCacheEntry *const *)right)->bnum;
    return (a > b) - (a < b);
}

// Order pointers for final writeback so the live hash and eviction chains
// remain valid while the storage and undo callbacks execute.
static inline int ClassicHfsFlushCache(ClassicHfsCacheEntry entries[ClassicHfsCacheSlots],
                                       int ordered, ClassicHfsWriteCachedSector store, void *context)
{
    ClassicHfsCacheEntry *pending[ClassicHfsCacheSlots];
    unsigned count = 0;
    for (unsigned i = 0; i < ClassicHfsCacheSlots; ++i)
        if (entries[i].flags & ClassicHfsCacheDirty)
            pending[count++] = &entries[i];
    if (ordered)
        qsort(pending, count, sizeof(pending[0]), ClassicHfsCompareCacheAddress);
    int result = 0;
    for (unsigned i = 0; i < count; ++i)
        if (ClassicHfsWriteBackEntry(pending[i], store, context) < 0)
            result = -1;
    return result;
}
