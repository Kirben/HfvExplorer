// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdlib.h>
#include <string.h>
#include "ClassicHfsGeometry.h"

typedef struct {
    hfsfile *live;
    hfsfile original;
} ClassicHfsFileSnapshot;

typedef struct {
    hfsvol original;
    block *bitmap;
    size_t bitmapBytes;
    char *extentsMap, *catalogMap;
    bcache *cache;
    ClassicHfsFileSnapshot *files;
    unsigned fileCount;
} ClassicHfsVolumeSnapshot;

static inline void *ClassicHfsCopyMemory(const void *source, size_t size)
{
    if (!source || !size)
        return NULL;
    void *copy = malloc(size);
    if (copy)
        memcpy(copy, source, size);
    return copy;
}

static inline void ClassicHfsReleaseSnapshot(ClassicHfsVolumeSnapshot *snapshot)
{
    free(snapshot->bitmap);
    free(snapshot->extentsMap);
    free(snapshot->catalogMap);
    free(snapshot->cache);
    free(snapshot->files);
}

// The caller supplies a zero-initialized snapshot and releases it on every exit.
static inline int ClassicHfsCaptureVolume(ClassicHfsVolumeSnapshot *snapshot, hfsvol *volume)
{
    snapshot->original = *volume;
    snapshot->bitmapBytes = sizeof(block) * ClassicHfsBitmapSectors(volume->mdb.drNmAlBlks);
    snapshot->bitmap = (block *)ClassicHfsCopyMemory(volume->vbm, snapshot->bitmapBytes);
    snapshot->extentsMap = (char *)ClassicHfsCopyMemory(volume->ext.map, volume->ext.mapsz);
    snapshot->catalogMap = (char *)ClassicHfsCopyMemory(volume->cat.map, volume->cat.mapsz);
    snapshot->cache = (bcache *)ClassicHfsCopyMemory(volume->cache, sizeof(bcache));
    if ((volume->vbm && !snapshot->bitmap) || (volume->ext.map && !snapshot->extentsMap) ||
        (volume->cat.map && !snapshot->catalogMap) || (volume->cache && !snapshot->cache))
        return -1;

    for (hfsfile *file = volume->files; file; file = file->next)
        if (++snapshot->fileCount > 65536)
            return -1;
    if (snapshot->fileCount) {
        snapshot->files = (ClassicHfsFileSnapshot *)malloc(snapshot->fileCount * sizeof(*snapshot->files));
        if (!snapshot->files)
            return -1;
        ClassicHfsFileSnapshot *entry = snapshot->files;
        for (hfsfile *file = volume->files; file; file = file->next, ++entry) {
            entry->live = file;
            entry->original = *file;
        }
    }
    return 0;
}

static inline void ClassicHfsRestoreVolume(ClassicHfsVolumeSnapshot *snapshot, hfsvol *volume)
{
    free(volume->ext.map);
    free(volume->cat.map);
    *volume = snapshot->original;
    volume->ext.map = snapshot->extentsMap;
    volume->cat.map = snapshot->catalogMap;
    snapshot->extentsMap = snapshot->catalogMap = NULL;
    if (snapshot->bitmap)
        memcpy(volume->vbm, snapshot->bitmap, snapshot->bitmapBytes);
    // Restore into the original cache allocation because its links point into itself.
    if (snapshot->cache)
        memcpy(volume->cache, snapshot->cache, sizeof(bcache));
    for (unsigned i = 0; i < snapshot->fileCount; ++i)
        *snapshot->files[i].live = snapshot->files[i].original;
}
