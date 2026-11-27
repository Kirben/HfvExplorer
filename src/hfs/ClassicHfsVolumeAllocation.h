// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <errno.h>

// Apply bitmap and master-directory changes together on a writable mount.
static inline int ClassicHfsCanChangeAllocation(const hfsvol *volume)
{
    if (volume->flags & HFS_READONLY) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return 0;
    }
    if ((volume->flags & HFS_WRITE_FAILED) || !volume->vbm) {
        ERROR(EIO, "classic HFS allocation map unavailable for writes");
        return 0;
    }
    return 1;
}

static inline int ClassicHfsClaimVolumeExtent(hfsvol *volume, ExtDescriptor *extent)
{
    if (!ClassicHfsCanChangeAllocation(volume))
        return -1;
    MDB *mdb = &volume->mdb;
    if (!mdb->drFreeBks) {
        unsigned first;
        if (ClassicHfsFindFreeRun(volume->vbm, mdb->drNmAlBlks, 0, 1, &first)) {
            ERROR(EIO, "classic HFS free count disagrees with allocation map");
        } else {
            ERROR(ENOSPC, "volume full");
        }
        return -1;
    }
    unsigned length = ClassicHfsAllocateBlocks(volume->vbm, mdb->drNmAlBlks,
                                              &mdb->drAllocPtr, &mdb->drFreeBks,
                                              extent->xdrNumABlks, &extent->xdrStABN);
    if (!length) {
        ERROR(EIO, "invalid classic HFS allocation map or free count");
        return -1;
    }
    extent->xdrNumABlks = (uint16_t)length;
    volume->flags |= HFS_UPDATE_MDB | HFS_UPDATE_VBM;
    return 0;
}

static inline int ClassicHfsReleaseVolumeExtent(hfsvol *volume, const ExtDescriptor *extent)
{
    if (!ClassicHfsCanChangeAllocation(volume))
        return -1;
    if (!ClassicHfsReleaseBlocks(volume->vbm, volume->mdb.drNmAlBlks, extent->xdrStABN,
                                 extent->xdrNumABlks, &volume->mdb.drFreeBks)) {
        ERROR(EIO, "invalid classic HFS extent release");
        return -1;
    }
    volume->flags |= HFS_UPDATE_MDB | HFS_UPDATE_VBM;
    return 0;
}
