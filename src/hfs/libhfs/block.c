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

# include <sys/types.h>

#ifdef WIN32
# include <io.h>
# include <windows.h>
# undef ERROR
# include <stdio.h>
#else
# include <unistd.h>
#endif

# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <time.h>

# ifdef DEBUG
# include <stdio.h>
# endif

# include "internal.h"
#ifdef WIN32
# include "../../ClassicHfsRecovery.h"
#endif
#include "../ClassicHfsTime.h"
#include "../ClassicHfsGeometry.h"
# include "block.h"
# include "low.h"

#include "../ClassicHfsSectorIO.h"

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

/*
 * NAME:	getblock()
 * DESCRIPTION:	read a block from the physical device
 */
static
int getblock(hfsvol *vol, unsigned long bnum, block *bp)
{
  hfv_offset_t offset;
  if (!ClassicHfsSectorOffset(vol->vstart, vol->vlen, bnum, 0, &offset)) {
    ERROR(EIO, "classic HFS read outside volume");
    return -1;
  }
  if (ClassicHfsTransferSector(vol->fd, offset, bp, 0, lseek, read, write) < 0) {
    ERROR(errno, "cannot read complete classic HFS sector");
    return -1;
  }
  return 0;
}

int ClassicHfsLoadRawSector(void *volume, unsigned long sector, block *bytes)
{
    return getblock((hfsvol *)volume, sector, bytes);
}

/*
 * NAME:	putblock()
 * DESCRIPTION:	write a block to the physical device
 */
static
int putblock(hfsvol *vol, unsigned long bnum, block *bp)
{
  hfv_offset_t offset;
  if (!ClassicHfsSectorOffset(vol->vstart, vol->vlen, bnum, 1, &offset)) {
    ERROR(EIO, "classic HFS write outside volume");
    return -1;
  }
  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled");return -1;}
  if(ClassicHfsCaptureBeforeWrite(vol,bnum)<0)return -1;
#ifdef WIN32
  if(hfv_hfs_before_sector_write(vol->fd,offset)<0){
    ERROR(EIO,"cannot publish classic HFS sector undo");
    return -1;
  }
#endif

  if (ClassicHfsTransferSector(vol->fd, offset, bp, 1, lseek, read, write) < 0) {
    ERROR(errno, "cannot write complete classic HFS sector");
    return -1;
  }

#ifdef WIN32
  hfv_hfs_after_sector_write(vol->fd);
#endif
  return 0;
}

/*
 * NAME:	getbucket()
 * DESCRIPTION:	fill a bucket's block buffer
 */
static
int getbucket(hfsvol *vol, bucket *b)
{
  if (getblock(vol, b->bnum, b->data) < 0)
    return -1;

  b->flags |=  HFS_BLOCK_INUSE;
  b->flags &= ~HFS_BLOCK_DIRTY;

  return 0;
}

/*
 * NAME:	putbucket()
 * DESCRIPTION:	store a bucket's block buffer
 */
int ClassicHfsStoreRawSector(void *volume, unsigned long sector, block *data)
{
  return putblock((hfsvol *)volume, sector, data);
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
        return writing ? putblock(volume, sector, bytes) : getblock(volume, sector, bytes);

    found = ClassicHfsFindCacheEntry(volume->cache, sector, &entry, ClassicHfsStoreRawSector, volume);
    if (found < 0)
        return -1;
    if (writing) {
        memcpy(entry->data, bytes, HFS_BLOCKSZ);
        entry->flags |= HFS_BLOCK_INUSE | HFS_BLOCK_DIRTY;
    } else {
        if (!found && getbucket(volume, entry) < 0)
            return -1;
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
