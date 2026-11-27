/*
 * hfsutils - tools for reading and writing Macintosh HFS volumes
 * Copyright (C) 1996, 1997 Robert Leslie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <errno.h>
#include <stdio.h>
#include "libhfs/internal.h"
#include "libhfs/block.h"
#include "ClassicHfsGeometry.h"
#include "ClassicHfsSectorIO.h"
#include "ClassicHfsTime.h"
#ifdef WIN32
#include "../ClassicHfsRecovery.h"
#endif

int ClassicHfsEnableVolumeCache(hfsvol *volume)
{
    if (ClassicHfsAcquireCache(&volume->cache) < 0) {
        ERROR(ENOMEM, "cannot allocate classic HFS sector cache");
        return -1;
    }
    return 0;
}

# ifdef DEBUG
/*
 * NAME:	b_debugcache()
 * DESCRIPTION:	dump the cache tables for a volume
 */
void b_debugcache(hfsvol *vol)
{
  bcache *cache = vol->cache;
  int i;
  bucket *b;

  printf("CACHE DUMP:\n");

  for (i = 0, b = cache->tail->cnext; i < HFS_CACHESZ; ++i, b = b->cnext)
    {
      if (b->flags & HFS_BLOCK_INUSE)
	{
	  printf("\t %lu", b->bnum);
	  if (b->flags & HFS_BLOCK_DIRTY)
	    printf("*");

	}
    }

  printf("\n");

  printf("HASH DUMP:\n");

  for (i = 0; i < ClassicHfsCacheHashSize; ++i)
    {
      int seen = 0;

      b = &cache->hash[i];

      while ((b = b->hnext))
	{
	  if (! seen)
	    printf("  %d:", i);

	  if (b->flags & HFS_BLOCK_INUSE)
	    {
	      printf(" %lu", b->bnum);
	      if (b->flags & HFS_BLOCK_DIRTY)
		printf("*");

	    }

	  seen = 1;
	}

      if (seen)
	printf("\n");
    }
}
# endif

int ClassicHfsLoadRawSector(void *context, unsigned long sector, block *bytes)
{
    hfsvol *volume = (hfsvol *)context;
    hfv_offset_t offset;
    if (!ClassicHfsSectorOffset(volume->vstart, volume->vlen, sector, 0, &offset)) {
        ERROR(EIO, "classic HFS read outside volume");
        return -1;
    }
    if (ClassicHfsTransferSector(volume->fd, offset, bytes, 0, fhook_lseek, fhook_read, fhook_write) < 0) {
        ERROR(errno, "cannot read complete classic HFS sector");
        return -1;
    }
    return 0;
}

int ClassicHfsStoreRawSector(void *context, unsigned long sector, block *bytes)
{
    hfsvol *volume = (hfsvol *)context;
    hfv_offset_t offset;
    if (!ClassicHfsSectorOffset(volume->vstart, volume->vlen, sector, 1, &offset)) {
        ERROR(EIO, "classic HFS write outside volume");
        return -1;
    }
    if (volume->flags & HFS_WRITE_FAILED) {
        ERROR(EIO, "Classic HFS writes are disabled");
        return -1;
    }
    if (volume->flags & HFS_READONLY) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if (ClassicHfsCaptureBeforeWrite(volume, sector) < 0)
        return -1;
#ifdef WIN32
    if (hfv_hfs_before_sector_write(volume->fd, offset) < 0) {
        ERROR(EIO, "cannot publish classic HFS sector undo");
        return -1;
    }
#endif
    if (ClassicHfsTransferSector(volume->fd, offset, bytes, 1, fhook_lseek, fhook_read, fhook_write) < 0) {
        ERROR(errno, "cannot write complete classic HFS sector");
        return -1;
    }
#ifdef WIN32
    hfv_hfs_after_sector_write(volume->fd);
#endif
    return 0;
}

int ClassicHfsFlushVolumeCache(hfsvol *volume)
{
    return ClassicHfsDrainCache(&volume->cache, 0, ClassicHfsStoreRawSector, volume);
}

int ClassicHfsCloseVolumeCache(hfsvol *volume)
{
# ifdef DEBUG
    if (volume->cache)
        b_debugcache(volume);
# endif
    return ClassicHfsDrainCache(&volume->cache, 1, ClassicHfsStoreRawSector, volume);
}

static int transfer_logical_sector(hfsvol *volume, unsigned long sector, block *bytes, int writing)
{
    hfv_offset_t offset;
    bucket *entry;
    int found;

    if (volume->flags & HFS_WRITE_FAILED) {
        ERROR(EIO, "Reopen/repair HFS after rollback failure");
        return -1;
    }
    if (writing && (volume->flags & HFS_READONLY)) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if (!ClassicHfsSectorOffset(volume->vstart, volume->vlen, sector, writing, &offset)) {
        ERROR(EIO, "logical sector outside classic HFS volume");
        return -1;
    }
    if (!volume->cache)
        return writing ? ClassicHfsStoreRawSector(volume, sector, bytes) : ClassicHfsLoadRawSector(volume, sector, bytes);

    found = ClassicHfsFindCacheEntry(volume->cache, sector, &entry, ClassicHfsStoreRawSector, volume);
    if (found < 0)
        return -1;
    if (writing) {
        memcpy(entry->data, bytes, HFS_BLOCKSZ);
        entry->flags |= HFS_BLOCK_INUSE | HFS_BLOCK_DIRTY;
    } else {
        if (!found) {
            if (ClassicHfsLoadRawSector(volume, sector, entry->data) < 0)
                return -1;
            entry->flags = ClassicHfsCacheValid;
        }
        memcpy(bytes, entry->data, HFS_BLOCKSZ);
    }
    return 0;
}

int ClassicHfsReadSector(hfsvol *volume, unsigned long sector, block *bytes)
{
    return transfer_logical_sector(volume, sector, bytes, 0);
}

int ClassicHfsWriteSector(hfsvol *volume, unsigned long sector, block *bytes)
{
    return transfer_logical_sector(volume, sector, bytes, 1);
}

int ClassicHfsTransferAllocationSector(hfsvol *volume, unsigned number, unsigned index,
                                       block *bytes, int writing)
{
    uint32_t sector;
    if (!ClassicHfsAllocationSector(volume->mdb.drAlBlSt, volume->mdb.drNmAlBlks,
                                    volume->lpa, volume->vlen, number, index, &sector)) {
        ERROR(EIO, "allocation sector outside classic HFS volume");
        return -1;
    }
    if (writing && (volume->flags & HFS_READONLY)) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if ((writing && !volume->vbm) || (volume->vbm && !ClassicHfsBitIsSet(volume->vbm, number))) {
        ERROR(EIO, "allocation sector is not marked in the classic HFS bitmap");
        return -1;
    }
    if (!writing)
        return ClassicHfsReadSector(volume, sector, bytes);
    if (ClassicHfsWriteSector(volume, sector, bytes) < 0)
        return -1;
    volume->mdb.drAtrb &= ~HFS_ATRB_UMOUNTED;
    volume->mdb.drLsMod = ClassicHfsFromUnixSeconds(time(0));
    ++volume->mdb.drWrCnt;
    volume->flags |= HFS_UPDATE_MDB;
    return 0;
}
