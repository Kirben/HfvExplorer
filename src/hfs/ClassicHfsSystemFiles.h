// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string.h>

// The master directory block describes the two B-tree files directly.
// They have a data fork but no parent directory or catalog entry of their own.
static inline void ClassicHfsInitSystemFile(hfsfile *file, hfsvol *volume,
                                            long id, const char *name, unsigned long length,
                                            unsigned long clump, const ExtDataRec extents)
{
    memset(file, 0, sizeof(*file));
    file->vol = volume;
    strcpy(file->name, name);
    file->cat.cdrType = cdrFilRec;
    file->cat.u.fil.filFlNum = id;
    file->cat.u.fil.filCrDat = volume->mdb.drCrDate;
    file->cat.u.fil.filMdDat = volume->mdb.drLsMod;
    file->cat.u.fil.filStBlk = extents[0].xdrStABN;
    file->cat.u.fil.filLgLen = length;
    file->cat.u.fil.filPyLen = length;
    memcpy(file->cat.u.fil.filExtRec, extents, sizeof(ExtDataRec));
    ClassicHfsSelectFork(file, 0);
    file->clump = clump;
}

static inline void ClassicHfsInitSystemFiles(hfsvol *volume)
{
    const MDB *mdb = &volume->mdb;
    ClassicHfsInitSystemFile(&volume->ext.f, volume, HFS_CNID_EXT, "extents overflow",
                             mdb->drXTFlSize, mdb->drXTClpSiz, mdb->drXTExtRec);
    ClassicHfsInitSystemFile(&volume->cat.f, volume, HFS_CNID_CAT, "catalog",
                             mdb->drCTFlSize, mdb->drCTClpSiz, mdb->drCTExtRec);
}
