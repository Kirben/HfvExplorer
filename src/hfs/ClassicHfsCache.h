// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { ClassicHfsCacheSlots = 128, ClassicHfsCacheValid = 1, ClassicHfsCacheDirty = 2 };

typedef struct ClassicHfsCacheEntry {
    int flags;
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

enum { ClassicHfsCacheHashSize = 32 };
typedef struct {
    ClassicHfsCacheEntry chain[ClassicHfsCacheSlots], hash[ClassicHfsCacheHashSize];
    ClassicHfsCacheEntry *tail;
    unsigned char pool[ClassicHfsCacheSlots][512];
} ClassicHfsCache;

static inline void ClassicHfsInitCache(ClassicHfsCache *cache)
{
    memset(cache, 0, sizeof(*cache));
    for (unsigned i = 0; i < ClassicHfsCacheSlots; ++i) {
        ClassicHfsCacheEntry *entry = &cache->chain[i];
        entry->data = &cache->pool[i];
        entry->cnext = &cache->chain[(i + 1) % ClassicHfsCacheSlots];
        entry->cprev = &cache->chain[(i + ClassicHfsCacheSlots - 1) % ClassicHfsCacheSlots];
    }
    cache->tail = &cache->chain[ClassicHfsCacheSlots - 1];
}

// The ring starts with the most recently used entry, immediately after tail.
// A miss reserves a slot; the caller marks it valid after filling its bytes.
static inline int ClassicHfsFindCacheEntry(ClassicHfsCache *cache, unsigned long sector,
                                           ClassicHfsCacheEntry **output,
                                           ClassicHfsWriteCachedSector store, void *context)
{
    ClassicHfsCacheEntry *head = &cache->hash[sector % ClassicHfsCacheHashSize];
    ClassicHfsCacheEntry *entry = head->hnext;
    while (entry && (entry->bnum != sector || !(entry->flags & ClassicHfsCacheValid)))
        entry = entry->hnext;
    int hit = entry != NULL;
    if (!hit) {
        entry = cache->tail;
        if (ClassicHfsWriteBackEntry(entry, store, context) < 0)
            return -1;
        if (entry->hprev)
            entry->hprev->hnext = entry->hnext;
        if (entry->hnext)
            entry->hnext->hprev = entry->hprev;
        entry->hnext = head->hnext;
        entry->hprev = head;
        if (head->hnext)
            head->hnext->hprev = entry;
        head->hnext = entry;
        entry->flags = 0;
        entry->bnum = sector;
    }
    if (entry == cache->tail)
        cache->tail = entry->cprev;
    else if (entry != cache->tail->cnext) {
        entry->cprev->cnext = entry->cnext;
        entry->cnext->cprev = entry->cprev;
        entry->cnext = cache->tail->cnext;
        entry->cprev = cache->tail;
        entry->cnext->cprev = entry;
        cache->tail->cnext = entry;
    }
    *output = entry;
    return hit;
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
