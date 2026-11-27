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
#include "ClassicHfsVolumeIO.h"
#include "ClassicHfsForkState.h"
#include "ClassicHfsTreeLookup.h"
#include "libhfs/volume.h"
#include "ClassicHfsVolumeBitmap.h"
#include "ClassicHfsTreeFlush.h"
#include "ClassicHfsBytes.h"
#include "ClassicHfsSystemFiles.h"
#include "ClassicHfsFormatTree.h"
#include "ClassicHfsGeometry.h"
#include "ClassicHfsSectorIO.h"
#include "ClassicHfsTime.h"
#ifdef WIN32
#include "../ClassicHfsRecovery.h"
#endif

int ClassicHfsSelectPartition(hfsvol *volume)
{
    block bytes;
    if (ClassicHfsReadSector(volume, 1, &bytes) < 0)
        return -1;
    unsigned count = ClassicHfsRead32(bytes + 4);
    if (!count || count > 4096 || (volume->vlen && count >= volume->vlen)) {
        ERROR(EINVAL, "invalid Apple partition count");
        return -1;
    }

    typedef struct { uint32_t start, length; } Range;
    Range *ranges = (Range *)calloc(count, sizeof(*ranges));
    if (!ranges) {
        ERROR(ENOMEM, "cannot allocate Apple partition ranges");
        return -1;
    }
    int remaining = volume->pnum;
    Range selected = {0};
    unsigned selectedIndex = count;
    int result = -1;
    for (unsigned i = 0; i < count; ++i) {
        if (i && ClassicHfsReadSector(volume, i + 1, &bytes) < 0)
            goto done;
        if (ClassicHfsRead16(bytes) != 0x504d || ClassicHfsRead32(bytes + 4) != count) {
            ERROR(EINVAL, "inconsistent Apple partition map");
            goto done;
        }
        Range range = {ClassicHfsRead32(bytes + 8), ClassicHfsRead32(bytes + 12)};
        ranges[i] = range;
        if (remaining > 0 && range.length && !strncmp((const char *)bytes + 48, "Apple_HFS", 32) &&
            --remaining == 0) {
            if (ClassicHfsRead32(bytes + 80) || range.start <= count ||
                (uint64_t)range.start + range.length > (volume->vlen ? volume->vlen : (uint64_t)UINT32_MAX + 1)) {
                ERROR(EINVAL, "invalid classic HFS partition range");
                goto done;
            }
            selected = range;
            selectedIndex = i;
        }
    }
    if (!selected.length) {
        ERROR(EINVAL, "cannot find requested classic HFS partition");
        goto done;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (i != selectedIndex && ranges[i].length &&
            ranges[i].start < (uint64_t)selected.start + selected.length &&
            selected.start < (uint64_t)ranges[i].start + ranges[i].length) {
            ERROR(EINVAL, "classic HFS partition overlaps another entry");
            goto done;
        }
    }
    if (!volume->vlen && ClassicHfsReadSector(volume, selected.start + (selected.length - 1), &bytes) < 0)
        goto done;
    volume->vstart = selected.start;
    volume->vlen = selected.length;
    result = 0;
done:
    free(ranges);
    return result;
}

int ClassicHfsLocateVolume(hfsvol *volume)
{
    block bytes;
    if (ClassicHfsReadSector(volume, 0, &bytes) < 0)
        return -1;
    const unsigned signature = ClassicHfsRead16(bytes);
    if (signature == 0x4552) {
        if (ClassicHfsRead16(bytes + 2) != HFS_BLOCKSZ) {
            ERROR(EINVAL, "unsupported block size");
            return -1;
        }
        const unsigned long sectors = ClassicHfsRead32(bytes + 4);
        if (sectors < 2) {
            ERROR(EINVAL, "classic HFS disk descriptor has no partition map sector");
            return -1;
        }
        const unsigned long previousLength = volume->vlen;
        volume->vlen = sectors;
        if (ClassicHfsSelectPartition(volume) < 0) {
            volume->vlen = previousLength;
            return -1;
        }
        return 0;
    }
    // Bootable floppies start at sector zero. Other media may have a map
    // without a driver descriptor, so retain the historical optional probe.
    if (signature != 0x4c4b) {
        if (ClassicHfsReadSector(volume, 1, &bytes) < 0)
            return -1;
        if (ClassicHfsRead16(bytes) == 0x504d)
            return ClassicHfsSelectPartition(volume);
    }
    volume->pnum = 0;
    return 0;
}

int ClassicHfsLoadVolumeHeader(hfsvol *volume)
{
    block bytes;
    if (ClassicHfsReadSector(volume, 2, &bytes) < 0)
        return -1;
    ClassicHfsDecodeMasterDirectory(bytes, &volume->mdb);
    volume->lpa = volume->mdb.drAlBlkSiz / HFS_BLOCKSZ;
    ClassicHfsInitSystemFiles(volume);
    return 0;
}

int ClassicHfsStoreVolumeHeader(hfsvol *volume)
{
    block bytes = {0};
    MDB header = volume->mdb;
    const hfsfile *catalog = &volume->cat.f;
    const hfsfile *extents = &volume->ext.f;
    int alternate = volume->flags & HFS_UPDATE_ALTMDB;

    // Reserve distinct sectors for the primary header, alternate, and final sector.
    if (alternate && volume->vlen < 5) {
        ERROR(EIO, "classic HFS volume too small for alternate master directory");
        return -1;
    }
    header.drXTFlSize = extents->cat.u.fil.filPyLen;
    header.drXTClpSiz = extents->clump;
    memcpy(header.drXTExtRec, extents->cat.u.fil.filExtRec, sizeof(header.drXTExtRec));
    header.drCTFlSize = catalog->cat.u.fil.filPyLen;
    header.drCTClpSiz = catalog->clump;
    memcpy(header.drCTExtRec, catalog->cat.u.fil.filExtRec, sizeof(header.drCTExtRec));

    ClassicHfsEncodeMasterDirectory(bytes, &header);
    if (ClassicHfsWriteSector(volume, 2, &bytes) < 0 ||
        (alternate && ClassicHfsWriteSector(volume, volume->vlen - 2, &bytes) < 0))
        return -1;
    volume->mdb = header;
    volume->flags &= ~(HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB);
    return 0;
}

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

void ClassicHfsReleaseVolume(hfsvol *volume)
{
    // Mount failures can reach here after allocating a cache but before
    // registering the volume. Disposal must never submit pending writes.
    free(volume->cache);
    free(volume->failure_message);
    free(volume->vbm);
    free(volume->ext.map);
    free(volume->cat.map);
    free(volume);
}

int ClassicHfsFlushVolumeMetadata(hfsvol *volume, int closing)
{
    if (volume->flags & HFS_READONLY)
        return 0;
    if ((volume->ext.flags & HFS_UPDATE_BTHDR) && ClassicHfsFlushTreeHeader(&volume->ext) < 0)
        return -1;
    if ((volume->cat.flags & HFS_UPDATE_BTHDR) && ClassicHfsFlushTreeHeader(&volume->cat) < 0)
        return -1;
    if ((volume->flags & HFS_UPDATE_VBM) && ClassicHfsFlushVolumeBitmap(volume) < 0)
        return -1;

    unsigned originalAttributes = volume->mdb.drAtrb;
    if (closing && !(originalAttributes & HFS_ATRB_UMOUNTED)) {
        volume->mdb.drAtrb |= HFS_ATRB_UMOUNTED;
        volume->flags |= HFS_UPDATE_MDB;
    }
    if ((volume->flags & (HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB)) && ClassicHfsStoreVolumeHeader(volume) < 0) {
        volume->mdb.drAtrb = originalAttributes;
        return -1;
    }
    return 0;
}

int hfs_format(char *path, int partition, char *name)
{
    size_t length = name ? strlen(name) : 0;
    if (!length || length > HFS_MAX_VLEN || strchr(name, ':') || partition < 0) {
        ERROR(EINVAL, "invalid classic HFS format name or partition");
        return -1;
    }

    block bitmap[16] = {0};
    hfsvol volume = {0};
    volume.vbm = bitmap;
    volume.cwd = HFS_CNID_ROOTDIR;
    volume.pnum = partition;
    volume.fd = ClassicHfsOpenVolumeDevice(path, O_RDWR);
    if (volume.fd < 0) {
        if (volume.fd != -2) {
            ERROR(errno, "error opening device for formatting");
        }
        return -1;
    }

    int result = -1;
    // Obtain capacity from the open descriptor. A failed sector read must
    // never be mistaken for the end of a smaller, writable device.
    hfv_offset_t size = fhook_lseek(volume.fd, 0, SEEK_END);
    if (size < 0) {
        ERROR(errno, "cannot determine classic HFS format capacity");
        goto done;
    }
    if ((uint64_t)size / HFS_BLOCKSZ > UINT32_MAX) {
        ERROR(EFBIG, "image exceeds classic HFS sector range");
        goto done;
    }
    volume.vlen = (uint32_t)((uint64_t)size / HFS_BLOCKSZ);
    if (partition && (!volume.vlen || ClassicHfsSelectPartition(&volume) < 0))
        goto done;
    ClassicHfsFormatGeometry geometry;
    if (!ClassicHfsPlanFormat(volume.vlen, &geometry)) {
        ERROR(EINVAL, "volume size must be >= 800K");
        goto done;
    }
    ClassicHfsEnableVolumeCache(&volume);
    volume.lpa = geometry.sectorsPerBlock;
    ClassicHfsInitMasterDirectory(&volume.mdb, &geometry, name, length, ClassicHfsFromUnixSeconds(time(NULL)));
    ClassicHfsInitSystemFiles(&volume);
    if (ClassicHfsInitFormatTree(&volume.ext, ClassicHfsExtentKeySize - 1, ClassicHfsCompareExtentKeys) < 0 ||
        ClassicHfsInitFormatTree(&volume.cat, 37, ClassicHfsCompareCatalogKeys) < 0 ||
        ClassicHfsReserveTreeSpace(&volume.ext, 1) < 0 || ClassicHfsReserveTreeSpace(&volume.cat, 1) < 0 ||
        ClassicHfsCreateDirectoryRecords(&volume, HFS_CNID_ROOTPAR, name) < 0)
        goto done;
    volume.mdb.drNxtCNID = 16;
    block boot = {0};
    if (ClassicHfsWriteSector(&volume, 0, &boot) < 0 || ClassicHfsWriteSector(&volume, 1, &boot) < 0)
        goto done;
    volume.flags |= HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB | HFS_UPDATE_VBM;
    if (ClassicHfsFlushVolumeMetadata(&volume, 1) == 0 && ClassicHfsCloseVolumeCache(&volume) == 0)
        result = 0;
done:
    free(volume.cache);
    free(volume.ext.map);
    free(volume.cat.map);
    if (fhook_close(volume.fd) < 0 && result == 0) {
        ERROR(errno, "error closing formatted classic HFS device");
        result = -1;
    }
    return result;
}
