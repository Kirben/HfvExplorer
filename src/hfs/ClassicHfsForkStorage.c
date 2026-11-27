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

# include <string.h>
# include <errno.h>

# include "libhfs/internal.h"
#include "ClassicHfsGeometry.h"
#include "ClassicHfsForkIO.h"
#include "ClassicHfsTime.h"
#include "ClassicHfsNodeIO.h"
# include "ClassicHfsVolumeIO.h"
# include "ClassicHfsForkState.h"
# include "ClassicHfsTreeLookup.h"
# include "libhfs/volume.h"
#include "ClassicHfsVolumeAllocation.h"

int ClassicHfsTransferTreeCursor(node *np, int writing)
{
  btree *bt = np->bt;
  ClassicHfsNodeStatus result = ClassicHfsTransferNode(np->nnum, bt->hdr.bthNNodes,
      bt->map, bt->mapsz, &np->data, &np->nd, np->roff, writing,
      writing ? ClassicHfsWriteForkSector : ClassicHfsReadForkSector, &bt->f);
  if (result == ClassicHfsNodeUnavailable)
    { ERROR(EIO, "classic HFS B-tree node outside allocated map"); }
  else if (result == ClassicHfsNodeBadOffsets)
    { ERROR(EIO, "invalid classic HFS B-tree node offsets"); }
  return result == ClassicHfsNodeOk ? 0 : -1;
}

// The extents file can describe its own overflow. Walk readable leaves when
// its root cannot be reached without first resolving the requested extent.
static int ClassicHfsFindOwnOverflow(btree *tree, unsigned char *key, node *leaf)
{
    unsigned long previous = 0;
    unsigned long number = tree->hdr.bthFNode;
    for (unsigned long visits = 0; number; ++visits) {
        if (visits >= tree->hdr.bthNNodes || number >= tree->hdr.bthNNodes) {
            ERROR(EIO, "invalid classic HFS overflow leaf chain");
            return -1;
        }
        leaf->bt = tree;
        leaf->nnum = number;
        if (ClassicHfsReadTreeNode(leaf) < 0)
            return -1;
        if ((unsigned char)leaf->nd.ndType != ndLeafNode || leaf->nd.ndNHeight != 1 || leaf->nd.ndBLink != previous) {
            ERROR(EIO, "invalid classic HFS overflow leaf link");
            return -1;
        }
        int found = ClassicHfsScanExtentLeaf(leaf->data, leaf->roff, leaf->nd.ndNRecs, key, &leaf->rnum);
        if (found == ClassicHfsBadExtentLeaf)
            { ERROR(EIO, "invalid classic HFS overflow leaf record"); }
        if (found != ClassicHfsContinueExtentScan)
            return found;
        previous = number;
        number = leaf->nd.ndFLink;
    }
    return 0;
}

static int ClassicHfsFindOverflow(hfsfile *file, unsigned first, ExtDataRec *data, node *leaf)
{
    node local;
    if (!leaf)
        leaf = &local;
    if (first > UINT16_MAX || (file->flags & HFS_EXT_BOOTSTRAP)) {
        ERROR(EIO, "invalid classic HFS overflow lookup");
        return -1;
    }
    unsigned char key[ClassicHfsExtentKeySize];
    ClassicHfsWriteExtentKey(key, file->fork, file->cat.u.fil.filFlNum, first);
    ExtDataRec saved;
    memcpy(saved, file->ext, sizeof(saved));
    unsigned savedStart = file->fabn;
    int found;
    if (file->cat.u.fil.filFlNum == HFS_CNID_EXT) {
        file->flags |= HFS_EXT_BOOTSTRAP;
        found = ClassicHfsFindOwnOverflow(&file->vol->ext, key, leaf);
        file->flags &= ~HFS_EXT_BOOTSTRAP;
    } else {
        found = ClassicHfsFindTreeKey(&file->vol->ext, key, leaf);
    }
    memcpy(file->ext, saved, sizeof(saved));
    file->fabn = savedStart;
    if (found <= 0)
        return found;
    unsigned payload = ClassicHfsExtentPayload(leaf->data, leaf->roff, leaf->nd.ndNRecs, leaf->rnum, key);
    if (!payload || (unsigned char)leaf->nd.ndType != ndLeafNode) {
        ERROR(EIO, "invalid classic HFS overflow lookup result");
        return -1;
    }
    if (data)
        ClassicHfsDecodeExtents(leaf->data + payload, *data);
    return 1;
}

static int ClassicHfsStoreOverflow(hfsfile *file, unsigned first, const ExtDataRec data, node *leaf)
{
    unsigned char key[ClassicHfsExtentKeySize];
    ClassicHfsWriteExtentKey(key, file->fork, file->cat.u.fil.filFlNum, first);
    unsigned payload = ClassicHfsExtentPayload(leaf->data, leaf->roff, leaf->nd.ndNRecs, leaf->rnum, key);
    if (!payload || first > UINT16_MAX || (unsigned char)leaf->nd.ndType != ndLeafNode || leaf->bt != &file->vol->ext) {
        ERROR(EIO, "invalid classic HFS overflow update");
        return -1;
    }
    ClassicHfsEncodeExtents(leaf->data + payload, data);
    return ClassicHfsWriteTreeNode(leaf);
}

/*
 * NAME:	file->doblock()
 * DESCRIPTION:	read or write a numbered block from a file
 */
static int load_fork_extents(void *context, unsigned first, ExtDataRec *record)
{
  return ClassicHfsFindOverflow((hfsfile *)context, first, record, 0);
}

static int ClassicHfsTransferForkSector(hfsfile *file, uint32_t number, block *bytes, int writing)
{
    ClassicHfsForkFields fork = ClassicHfsSelectedFork(file);
    if (number >= *fork.physical / HFS_BLOCKSZ || *fork.physical % HFS_BLOCKSZ) {
        ERROR(EIO, "sector outside classic HFS physical fork");
        return -1;
    }
    unsigned physical, sector;
    int mapped = ClassicHfsResolveForkSector(number, file->vol->lpa, file->vol->mdb.drNmAlBlks,
        *fork.extents, file->ext, &file->fabn, load_fork_extents, file, &physical, &sector);
    if (mapped < 0)
        { ERROR(EIO, "invalid classic HFS fork extent or sector"); }
    if (mapped <= 0)
        return -1;
    return ClassicHfsTransferAllocationSector(file->vol, physical, sector, bytes, writing);
}

int ClassicHfsReadForkSector(void *file, uint32_t sector, block *bytes)
{
    return ClassicHfsTransferForkSector((hfsfile *)file, sector, bytes, 0);
}

int ClassicHfsWriteForkSector(void *file, uint32_t sector, block *bytes)
{
    return ClassicHfsTransferForkSector((hfsfile *)file, sector, bytes, 1);
}

// Validate every record from the cached logical position to physical EOF
// before claiming volume blocks. Reload the cached record from its owner.
static int ClassicHfsFindAppendRecord(hfsfile *file, unsigned end, unsigned *start,
                                      ExtDataRec tail, node *leaf)
{
    unsigned volumeBlocks = file->vol->mdb.drNmAlBlks;
    *start = file->fabn;
    if (end > volumeBlocks || (*start && *start >= end)) {
        ERROR(EIO, "invalid classic HFS fork extent position");
        return -1;
    }
    for (;;) {
        if (*start) {
            if (ClassicHfsFindOverflow(file, *start, tail, leaf) <= 0)
                return -1;
        } else {
            memcpy(tail, *ClassicHfsSelectedFork(file).extents, sizeof(ExtDataRec));
        }
        if (!end) {
            for (unsigned i = 0; i < ClassicHfsExtentCount; ++i)
                if (tail[i].xdrNumABlks) {
                    ERROR(EIO, "empty classic HFS fork has allocated extents");
                    return -1;
                }
            return 0;
        }
        ClassicHfsTruncation checked;
        if (!ClassicHfsPlanTruncation(tail, *start, end, end, volumeBlocks, &checked)) {
            ERROR(EIO, "invalid classic HFS extent record during fork growth");
            return -1;
        }
        if (checked.next == end)
            return 0;
        *start = checked.next;
    }
}

int ClassicHfsExtendForkAllocation(hfsfile *file)
{
  hfsvol *vol = file->vol;
  ExtDescriptor blocks;
  ClassicHfsForkFields fields = ClassicHfsSelectedFork(file);
  unsigned long clump = file->clump ? file->clump : vol->mdb.drClpSiz;
  int sizeStatus = ClassicHfsGrowthRequest(*fields.physical, vol->mdb.drAlBlkSiz, clump,
                                           &blocks.xdrNumABlks);
  if (sizeStatus != ClassicHfsForkSizeOk) {
    if (sizeStatus == ClassicHfsForkSizeLimit)
      { ERROR(EFBIG, "classic HFS physical fork size limit"); }
    else
      { ERROR(EIO, "invalid physical fork length"); }
    return -1;
  }
  unsigned start, end = *fields.physical / vol->mdb.drAlBlkSiz;
  ExtDataRec tail;
  node leaf;
  if (ClassicHfsFindAppendRecord(file, end, &start, tail, &leaf) < 0 ||
      ClassicHfsClaimVolumeExtent(vol, &blocks) < 0)
    return -1;

  ExtDataRec updated;
  int append = ClassicHfsAppendExtent(tail, start, end, vol->mdb.drNmAlBlks,
                                      blocks, updated);
  if (append == ClassicHfsCannotAppend) {
    ClassicHfsReleaseVolumeExtent(vol, &blocks);
    ERROR(EIO, "inconsistent classic HFS fork extents at physical EOF");
    return -1;
  }
  if (append == ClassicHfsNewOverflowRecord) {
    unsigned char record[HFS_EXTRECMAXLEN];
    int reclen = ClassicHfsWriteExtentKey(record, file->fork, file->cat.u.fil.filFlNum, end);
    reclen += ClassicHfsEncodeExtents(record + reclen, updated);
    if (ClassicHfsInsertTreeRecord(&vol->ext, record, reclen) < 0) {
      ClassicHfsReleaseVolumeExtent(vol, &blocks);
      return -1;
    }
    start = end;
  } else if (start) {
    if (ClassicHfsStoreOverflow(file, start, updated, &leaf) < 0) {
      ClassicHfsReleaseVolumeExtent(vol, &blocks);
      return -1;
    }
  } else {
    memcpy(*fields.extents, updated, sizeof(updated));
  }
  file->fabn = start;
  memcpy(file->ext, updated, sizeof(updated));
  *fields.physical += blocks.xdrNumABlks * vol->mdb.drAlBlkSiz;
  file->flags |= HFS_UPDATE_CATREC;

  return blocks.xdrNumABlks;
}

// Validate retained and discarded records together before releasing any
// block. A corrupt overflow record can otherwise free a retained extent.
static int validate_truncation_chain(hfsfile *file, unsigned end)
{
    unsigned char seen[(UINT16_MAX + 7u) / 8u] = {0};
    for (unsigned first = 0; first < end;) {
        ExtDataRec record;
        if (!first)
            memcpy(record, *ClassicHfsSelectedFork(file).extents, sizeof(record));
        else if (ClassicHfsFindOverflow(file, first, &record, NULL) <= 0)
            return -1;
        ClassicHfsTruncation plan;
        if (!ClassicHfsPlanTruncation(record, first, end, end, file->vol->mdb.drNmAlBlks, &plan))
            break;
        for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
            unsigned start = record[i].xdrStABN;
            unsigned limit = start + record[i].xdrNumABlks;
            for (unsigned n = start; n < limit; ++n) {
                if (!file->vol->vbm || !ClassicHfsBitIsSet(file->vol->vbm, n) || ClassicHfsBitIsSet(seen, n)) {
                    ERROR(EIO, "overlapping or unallocated classic HFS truncation extent");
                    return -1;
                }
                ClassicHfsSetBit(seen, n);
            }
        }
        first = plan.next;
        if (first == end)
            return 0;
    }
    ERROR(EIO, "invalid classic HFS truncation chain");
    return -1;
}

int ClassicHfsTrimForkAllocation(hfsfile *file)
{
  ClassicHfsForkFields fields = ClassicHfsSelectedFork(file);
  ExtDataRec *extrec = fields.extents;
  unsigned long *pylen = fields.physical;
  unsigned long alblksz = file->vol->mdb.drAlBlkSiz;
  uint32_t newpylen;
  int sizeStatus = ClassicHfsTruncatedSize(*fields.logical, *pylen, alblksz, &newpylen);
  if (sizeStatus != ClassicHfsForkSizeOk) {
    if (sizeStatus == ClassicHfsForkSizeLimit)
      { ERROR(EFBIG, "classic HFS physical fork size limit"); }
    else
      { ERROR(EIO, "invalid physical fork length"); }
    return -1;
  }
  if (newpylen == *pylen)
    return 0;

  unsigned oldEnd = *pylen / alblksz, keep = newpylen / alblksz;
  if (validate_truncation_chain(file, oldEnd) < 0)
    return -1;
  unsigned start = 0;
  while (start < oldEnd) {
    ExtDataRec current;
    node n;
    if (start) {
      if (ClassicHfsFindOverflow(file, start, &current, &n) <= 0)
        return -1;
    } else
      memcpy(current, *extrec, sizeof(current));

    ClassicHfsTruncation plan;
    if (!ClassicHfsPlanTruncation(current, start, keep, oldEnd,
                                  file->vol->mdb.drNmAlBlks, &plan)) {
      ERROR(EIO, "invalid classic HFS fork truncation extent");
      return -1;
    }
    if (plan.next > keep) {
      for (unsigned i = 0; i < ClassicHfsExtentCount; ++i)
        if (plan.released[i].xdrNumABlks && ClassicHfsReleaseVolumeExtent(file->vol, &plan.released[i]) < 0)
          return -1;
      if (!start)
        memcpy(*extrec, plan.kept, sizeof(plan.kept));
      else if (plan.kept[0].xdrNumABlks) {
        if (ClassicHfsStoreOverflow(file, start, plan.kept, &n) < 0)
          return -1;
      } else if (ClassicHfsDeleteTreeKey(&file->vol->ext, HFS_NODEREC(n, n.rnum)) < 0)
        return -1;
    }
    start = plan.next;
  }
  memcpy(file->ext, *extrec, sizeof(file->ext));
  file->fabn = 0;
  *pylen = newpylen;
  file->flags |= HFS_UPDATE_CATREC;

  return 0;
}

int ClassicHfsFlushOpenFile(hfsfile *file)
{
    if ((file->vol->flags & HFS_READONLY) || !(file->flags & HFS_UPDATE_CATREC))
        return 0;
    node leaf;
    CatDataRec disk, update;
    if (ClassicHfsFindCatalogEntry(file->vol, file->parid, file->name, &disk, NULL, &leaf) <= 0)
        return -1;
    if (!ClassicHfsPrepareFileUpdate(&disk, &file->cat, file->clump, &update)) {
        ERROR(EIO, "classic HFS open file no longer matches its catalog entry");
        return -1;
    }
    if (ClassicHfsStoreCatalogEntry(&update, &leaf) < 0)
        return -1;
    file->cat = update;
    file->flags &= ~HFS_UPDATE_CATREC;
    return 0;
}

/* High-Level File Routines ================================================ */

hfsfile *hfs_open(hfsvol *vol, char *path)
{
  hfsfile *file;

  if (ClassicHfsCurrentVolume(&vol) < 0)
    return 0;

  file = ALLOC(hfsfile, 1);
  if (file == 0)
    {
      ERROR(ENOMEM, 0);
      return 0;
    }

  if (ClassicHfsResolvePath(vol, path, &file->cat, &file->parid, file->name, 0) <= 0)
    {
      FREE(file);
      return 0;
    }

  if (file->cat.cdrType != cdrFilRec)
    {
      FREE(file);
      ERROR(EISDIR, 0);
      return 0;
    }

  if (file->cat.u.fil.filLgLen > HFS_MAX_FORK_SIZE || file->cat.u.fil.filRLgLen > HFS_MAX_FORK_SIZE) {
    FREE(file); ERROR(EFBIG, "classic HFS fork exceeds Apple's 2 GiB minus one limit"); return 0;
  }
  file->vol   = vol;
  file->clump = file->cat.u.fil.filClpSize;
  file->flags = 0;

  ClassicHfsSelectFork(file, 0);

  file->prev = 0;
  file->next = vol->files;

  if (vol->files)
    vol->files->prev = file;

  vol->files = file;

  return file;
}

static int fork_needs_trim(hfsfile *file)
{
    ClassicHfsForkFields fields = ClassicHfsSelectedFork(file);
    uint32_t rounded;
    int status = ClassicHfsTruncatedSize(*fields.logical, *fields.physical,
                                         file->vol->mdb.drAlBlkSiz, &rounded);
    if (status != ClassicHfsForkSizeOk) {
        ERROR(status == ClassicHfsForkSizeLimit ? EFBIG : EIO, "invalid classic HFS fork size");
        return -1;
    }
    return rounded != *fields.physical;
}

int hfs_setfork(hfsfile *file, int fork)
{
  if (file->vol->flags & HFS_WRITE_FAILED) {
    ERROR(EIO, "classic HFS volume requires repair");
    return -1;
  }
  if (!(file->vol->flags & HFS_READONLY)) {
    int trim = fork_needs_trim(file);
    if (trim < 0)
      return -1;
    if (trim) {
      if (ClassicHfsBeginTransaction(file->vol) < 0)
        return -1;
      int result = ClassicHfsTrimForkAllocation(file);
      if (ClassicHfsEndTransaction(file->vol, result) < 0)
        return -1;
    }
  }
  ClassicHfsSelectFork(file, fork);
  return 0;
}

int hfs_getfork(hfsfile *file)
{
  return file->fork != fkData;
}

long hfs_read(hfsfile *file, void *buf, unsigned long len)
{
  unsigned long *logicalLength;
  logicalLength = ClassicHfsSelectedFork(file).logical;
  return ClassicHfsReadFork(file, ClassicHfsReadForkSector, *logicalLength, &file->pos, buf, len);
}

static int grow_fork(void *context)
{
  hfsfile *file = (hfsfile *)context;
  if (ClassicHfsReserveTreeSpace(&file->vol->ext, 1) < 0)
    return -1;
  return ClassicHfsExtendForkAllocation(file);
}

long hfs_write(hfsfile *file, void *buf, unsigned long len)
{
  unsigned long *lglen, *pylen;

  if (file->vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  ClassicHfsForkFields forkFields = ClassicHfsSelectedFork(file);
  lglen = forkFields.logical;
  pylen = forkFields.physical;

  if (len > 0x7fffffffUL) { ERROR(EINVAL, "transfer count exceeds signed API range"); return -1; }
  if (file->pos > HFS_MAX_FORK_SIZE || len > HFS_MAX_FORK_SIZE - file->pos) { ERROR(EFBIG, "classic HFS fork exceeds Apple limit"); return -1; }
  if (len)
    {
      file->cat.u.fil.filMdDat = ClassicHfsFromUnixSeconds(time(0));
      file->flags |= HFS_UPDATE_CATREC;
    }

  return ClassicHfsWriteFork(file, ClassicHfsReadForkSector, ClassicHfsWriteForkSector, grow_fork,
                             pylen, lglen, &file->pos, buf, len);
}

static int hfs_truncate_impl(hfsfile *file, unsigned long len)
{
  if (len > HFS_MAX_FORK_SIZE) { ERROR(EFBIG, "classic HFS fork exceeds Apple limit"); return -1; }
  unsigned long *lglen;

  lglen = ClassicHfsSelectedFork(file).logical;

  if (*lglen > len)
    {
      if (file->vol->flags & HFS_READONLY)
	{
	  ERROR(EROFS, 0);
	  return -1;
	}

      *lglen = len;

      file->cat.u.fil.filMdDat = ClassicHfsFromUnixSeconds(time(0));
      file->flags |= HFS_UPDATE_CATREC;

      if (file->pos > len)
	file->pos = len;
    }

  return 0;
}

hfv_offset_t hfs_lseek(hfsfile *file, hfv_offset_t offset, int from)
{
  unsigned long *lglen;
  lglen = ClassicHfsSelectedFork(file).logical;
  if (!ClassicHfsSeekFork(*lglen, &file->pos, offset, from)) {
    ERROR(EINVAL, 0);
    return -1;
  }
  return file->pos;
}

int hfs_close(hfsfile *file)
{
  hfsvol *vol=file->vol;int result=0;
  if(vol->flags&HFS_WRITE_FAILED)result=-1;
  else if(!(vol->flags&HFS_READONLY)) {
    int trim = fork_needs_trim(file);
    if (trim < 0)
      return -1;
    if ((file->flags & HFS_UPDATE_CATREC) || trim) {
      if(ClassicHfsBeginTransaction(vol)<0) {
        if(!(vol->flags&HFS_WRITE_FAILED))return -1; // Keep the view for retry.
        result=-1;
      } else {
        if(ClassicHfsTrimForkAllocation(file)<0||ClassicHfsFlushOpenFile(file)<0)result=-1;
        result=ClassicHfsEndTransaction(vol,result);
        if(result<0&&!(vol->flags&HFS_WRITE_FAILED))return -1;
      }
    }
  }
  if(file->prev)file->prev->next=file->next;
  if(file->next)file->next->prev=file->prev;
  if(file==vol->files)vol->files=file->next;
  FREE(file);return result;
}

int hfs_truncate(hfsfile *file,unsigned long length) {
 unsigned long *logical;int result;hfsvol *vol;
 /* Preserve the public range check before touching a caller-supplied file. */
 if(length>HFS_MAX_FORK_SIZE){ERROR(EFBIG,"classic HFS fork exceeds Apple limit");return -1;}
 logical = ClassicHfsSelectedFork(file).logical;
 if(*logical<=length)return 0;
 vol=file->vol;
 if(ClassicHfsBeginTransaction(vol)<0)return -1;
 result=hfs_truncate_impl(file,length);
 if(result==0&&ClassicHfsTrimForkAllocation(file)<0)result=-1;
 if(result==0&&ClassicHfsFlushOpenFile(file)<0)result=-1;
 return ClassicHfsEndTransaction(vol,result);
}
