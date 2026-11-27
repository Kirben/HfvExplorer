// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsVolumeIO.h"

static inline int ClassicHfsVolumeBitmapLength(const hfsvol *volume)
{
    unsigned sectors = ClassicHfsBitmapSectors(volume->mdb.drNmAlBlks);
    if (!ClassicHfsBitmapFits(volume->mdb.drVBMSt, sectors, volume->mdb.drAlBlSt, volume->vlen)) {
        ERROR(EIO, "invalid classic HFS volume bitmap range");
        return -1;
    }
    return (int)sectors;
}

static inline int ClassicHfsLoadVolumeBitmap(hfsvol *volume)
{
    int sectors = ClassicHfsVolumeBitmapLength(volume);
    if (sectors < 0)
        return -1;
    block *bitmap = (block *)calloc(sectors, sizeof(*bitmap));
    if (!bitmap) {
        ERROR(ENOMEM, "cannot allocate classic HFS volume bitmap");
        return -1;
    }
    for (int i = 0; i < sectors; ++i) {
        if (ClassicHfsReadSector(volume, volume->mdb.drVBMSt + i, bitmap + i) < 0) {
            free(bitmap);
            return -1;
        }
    }
    volume->vbm = bitmap;
    return 0;
}

static inline int ClassicHfsFlushVolumeBitmap(hfsvol *volume)
{
    int sectors = ClassicHfsVolumeBitmapLength(volume);
    if (sectors < 0)
        return -1;
    if (!volume->vbm) {
        ERROR(EIO, "missing classic HFS volume bitmap");
        return -1;
    }
    for (int i = 0; i < sectors; ++i) {
        if (ClassicHfsWriteSector(volume, volume->mdb.drVBMSt + i, volume->vbm + i) < 0)
            return -1;
    }
    volume->flags &= ~HFS_UPDATE_VBM;
    return 0;
}
