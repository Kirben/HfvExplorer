// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string.h>

enum { fkData = 0, fkRsrc = 0xff };

typedef struct {
    unsigned long *logical;
    unsigned long *physical;
    ExtDataRec *extents;
} ClassicHfsForkFields;

static inline ClassicHfsForkFields ClassicHfsSelectedFork(hfsfile *file)
{
    if (file->fork == fkData) {
        ClassicHfsForkFields data = { &file->cat.u.fil.filLgLen, &file->cat.u.fil.filPyLen,
                                      &file->cat.u.fil.filExtRec };
        return data;
    }
    ClassicHfsForkFields resource = { &file->cat.u.fil.filRLgLen, &file->cat.u.fil.filRPyLen,
                                      &file->cat.u.fil.filRExtRec };
    return resource;
}

static inline void ClassicHfsSelectFork(hfsfile *file, int resource)
{
    file->fork = resource ? fkRsrc : fkData;
    memcpy(file->ext, *ClassicHfsSelectedFork(file).extents, sizeof(file->ext));
    file->fabn = 0;
    file->pos = 0;
}
