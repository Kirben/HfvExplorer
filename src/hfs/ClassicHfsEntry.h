// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsCatalogRecord.h"
#include "ClassicHfsTime.h"
#include "ClassicHfs.h"

static inline void ClassicHfsDescribeEntry(long parent, const char *name,
                                           const ClassicHfsCatalogRecord *record, hfsdirent *entry)
{
    strcpy(entry->name, name);
    entry->parid = parent;
    if (record->cdrType == cdrDirRec) {
        const ClassicHfsDirectory *item = &record->u.dir;
        entry->flags = HFS_ISDIR;
        entry->cnid = item->dirDirID;
        entry->crdate = ClassicHfsToUnixSeconds(item->dirCrDat);
        entry->mddate = ClassicHfsToUnixSeconds(item->dirMdDat);
        entry->bkdate = ClassicHfsToUnixSeconds(item->dirBkDat);
        entry->u.dir.valence = item->dirVal;
        entry->fdflags = item->dirUsrInfo.frFlags;
        entry->fdlocation.v = item->dirUsrInfo.frLocation.v;
        entry->fdlocation.h = item->dirUsrInfo.frLocation.h;
        entry->u.dir.rect.top = item->dirUsrInfo.frRect.top;
        entry->u.dir.rect.left = item->dirUsrInfo.frRect.left;
        entry->u.dir.rect.bottom = item->dirUsrInfo.frRect.bottom;
        entry->u.dir.rect.right = item->dirUsrInfo.frRect.right;
    }
    else if (record->cdrType == cdrFilRec) {
        const ClassicHfsCatalogFile *item = &record->u.fil;
        entry->flags = (item->filFlags & 1) ? HFS_ISLOCKED : 0;
        entry->cnid = item->filFlNum;
        entry->crdate = ClassicHfsToUnixSeconds(item->filCrDat);
        entry->mddate = ClassicHfsToUnixSeconds(item->filMdDat);
        entry->bkdate = ClassicHfsToUnixSeconds(item->filBkDat);
        entry->u.file.dsize = item->filLgLen;
        entry->u.file.rsize = item->filRLgLen;
        ClassicHfsWrite32((unsigned char *)entry->u.file.type, item->filUsrWds.fdType);
        entry->u.file.type[4] = 0;
        ClassicHfsWrite32((unsigned char *)entry->u.file.creator, item->filUsrWds.fdCreator);
        entry->u.file.creator[4] = 0;
        entry->fdflags = item->filUsrWds.fdFlags;
        entry->fdlocation.v = item->filUsrWds.fdLocation.v;
        entry->fdlocation.h = item->filUsrWds.fdLocation.h;
    }
}

static inline void ClassicHfsApplyEntry(ClassicHfsCatalogRecord *record, const hfsdirent *entry)
{
    if (record->cdrType == cdrDirRec) {
        ClassicHfsDirectory *item = &record->u.dir;
        item->dirCrDat = ClassicHfsFromUnixSeconds(entry->crdate);
        item->dirMdDat = ClassicHfsFromUnixSeconds(entry->mddate);
        item->dirBkDat = ClassicHfsFromUnixSeconds(entry->bkdate);
        item->dirUsrInfo.frFlags = entry->fdflags;
        item->dirUsrInfo.frLocation.v = entry->fdlocation.v;
        item->dirUsrInfo.frLocation.h = entry->fdlocation.h;
        item->dirUsrInfo.frRect.top = entry->u.dir.rect.top;
        item->dirUsrInfo.frRect.left = entry->u.dir.rect.left;
        item->dirUsrInfo.frRect.bottom = entry->u.dir.rect.bottom;
        item->dirUsrInfo.frRect.right = entry->u.dir.rect.right;
    }
    else if (record->cdrType == cdrFilRec) {
        ClassicHfsCatalogFile *item = &record->u.fil;
        item->filFlags = (item->filFlags & ~1) | ((entry->flags & HFS_ISLOCKED) != 0);
        item->filCrDat = ClassicHfsFromUnixSeconds(entry->crdate);
        item->filMdDat = ClassicHfsFromUnixSeconds(entry->mddate);
        item->filBkDat = ClassicHfsFromUnixSeconds(entry->bkdate);
        item->filUsrWds.fdType = ClassicHfsRead32((const unsigned char *)entry->u.file.type);
        item->filUsrWds.fdCreator = ClassicHfsRead32((const unsigned char *)entry->u.file.creator);
        item->filUsrWds.fdFlags = entry->fdflags;
        item->filUsrWds.fdLocation.v = entry->fdlocation.v;
        item->filUsrWds.fdLocation.h = entry->fdlocation.h;
    }
}
