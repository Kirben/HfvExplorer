// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string.h>
#include <stdlib.h>

typedef struct {
    unsigned long sector;
    unsigned next;
    unsigned char before[512];
} ClassicHfsUndoRecord;

typedef int (*ClassicHfsUndoSectorIO)(void *, unsigned long, unsigned char (*)[512]);
typedef int (*ClassicHfsUndoSync)(void *);

enum { ClassicHfsUndoBuckets = 2048, ClassicHfsUndoLimit = 65536 };
typedef struct {
    ClassicHfsUndoRecord *records;
    unsigned count, capacity, buckets[ClassicHfsUndoBuckets];
} ClassicHfsSectorUndo;

typedef enum {
    ClassicHfsCaptured, ClassicHfsCaptureReadFailed,
    ClassicHfsCaptureLimit, ClassicHfsCaptureNoMemory
} ClassicHfsCaptureResult;

static inline ClassicHfsCaptureResult ClassicHfsCaptureSector(ClassicHfsSectorUndo *undo,
    unsigned long sector, void *context, ClassicHfsUndoSectorIO readSector)
{
    unsigned bucket = (sector ^ (sector >> 11)) % ClassicHfsUndoBuckets;
    for (unsigned i = undo->buckets[bucket]; i; i = undo->records[i - 1].next)
        if (undo->records[i - 1].sector == sector)
            return ClassicHfsCaptured;
    if (undo->count == ClassicHfsUndoLimit)
        return ClassicHfsCaptureLimit;
    if (undo->count == undo->capacity) {
        unsigned capacity = undo->capacity ? undo->capacity * 2 : 128;
        ClassicHfsUndoRecord *records = (ClassicHfsUndoRecord *)
            realloc(undo->records, capacity * sizeof(*records));
        if (!records)
            return ClassicHfsCaptureNoMemory;
        undo->records = records;
        undo->capacity = capacity;
    }
    ClassicHfsUndoRecord *record = &undo->records[undo->count];
    if (readSector(context, sector, &record->before) < 0)
        return ClassicHfsCaptureReadFailed;
    record->sector = sector;
    record->next = undo->buckets[bucket];
    undo->buckets[bucket] = ++undo->count;
    return ClassicHfsCaptured;
}

// Attempt every original sector even if one restoration fails. The caller
// retains all records as recovery evidence until verification and sync finish.
static inline int ClassicHfsRestoreSectors(ClassicHfsUndoRecord *records, unsigned count,
                                          void *context, ClassicHfsUndoSectorIO writeSector,
                                          ClassicHfsUndoSectorIO readSector, ClassicHfsUndoSync sync)
{
    unsigned char actual[512];
    int result = 0;
    for (unsigned i = count; i > 0; --i) {
        ClassicHfsUndoRecord *record = &records[i - 1];
        if (writeSector(context, record->sector, &record->before) < 0 ||
            readSector(context, record->sector, &actual) < 0 ||
            memcmp(actual, record->before, sizeof(actual)))
            result = -1;
    }
    if (count && sync(context) < 0)
        result = -1;
    return result;
}
