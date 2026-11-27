// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string.h>

typedef struct {
    unsigned long sector;
    unsigned next;
    unsigned char before[512];
} ClassicHfsUndoRecord;

typedef int (*ClassicHfsUndoSectorIO)(void *, unsigned long, unsigned char (*)[512]);
typedef int (*ClassicHfsUndoSync)(void *);

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
