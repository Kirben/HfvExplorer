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

# include "internal.h"
#include "../ClassicHfsGeometry.h"
# include "block.h"
# include "file.h"
# include "btree.h"
# include "volume.h"

/*
 * NAME:	file->doblock()
 * DESCRIPTION:	read or write a numbered block from a file
 */
static int load_fork_extents(void *context, unsigned first, ExtDataRec *record)
{
  return v_extsearch((hfsfile *)context, first, record, 0);
}

int f_doblock(hfsfile *file, unsigned long num, block *bp,
	       int (*func)(hfsvol *, unsigned int, unsigned int, block *))
{
  ExtDataRec *initial;
  unsigned physical, sector;
  initial = ClassicHfsSelectedFork(file).extents;
  int mapped = ClassicHfsResolveForkSector(num, file->vol->lpa, file->vol->mdb.drNmAlBlks,
      *initial, file->ext, &file->fabn, load_fork_extents, file, &physical, &sector);
  if (mapped < 0)
    { ERROR(EIO, "invalid classic HFS fork extent or sector"); }
  return mapped > 0 ? func(file->vol, physical, sector, bp) : -1;
}

/*
 * NAME:	file->alloc()
 * DESCRIPTION:	reserve disk blocks for a file
 */
int f_alloc(hfsfile *file)
{
  hfsvol *vol = file->vol;
  ExtDescriptor blocks;
  ExtDataRec *extrec;
  unsigned long *pylen, clumpsz;
  unsigned int start, end;
  node n;
  int i;

  clumpsz = file->clump;
  if (clumpsz == 0)
    clumpsz = vol->mdb.drClpSiz;

  pylen = ClassicHfsSelectedFork(file).physical;
  int sizeStatus = ClassicHfsGrowthRequest(*pylen, vol->mdb.drAlBlkSiz, clumpsz,
                                          &blocks.xdrNumABlks);
  if (sizeStatus != ClassicHfsForkSizeOk) {
    if (sizeStatus == ClassicHfsForkSizeLimit)
      { ERROR(EFBIG, "classic HFS physical fork size limit"); }
    else
      { ERROR(EIO, "invalid physical fork length"); }
    return -1;
  }

  if (v_allocblocks(vol, &blocks) < 0)
    return -1;

  /* update the file's extents */

  extrec = ClassicHfsSelectedFork(file).extents;

  start  = file->fabn;
  end    = *pylen / vol->mdb.drAlBlkSiz;

  n.nnum = 0;
  i      = -1;

  while (start < end)
    {
      for (i = 0; i < 3; ++i)
	{
	  unsigned int num;

	  num    = file->ext[i].xdrNumABlks;
	  start += num;

	  if (start == end)
	    break;
	  else if (start > end)
	    {
	      v_freeblocks(vol, &blocks);
	      ERROR(EIO, "file extents exceed file physical length");
	      return -1;
	    }
	  else if (num == 0)
	    {
	      v_freeblocks(vol, &blocks);
	      ERROR(EIO, "empty file extent");
	      return -1;
	    }
	}

      if (start == end)
	break;

      if (v_extsearch(file, start, &file->ext, &n) <= 0)
	{
	  v_freeblocks(vol, &blocks);
	  return -1;
	}

      file->fabn = start;
    }

  ExtDataRec updated;
  int append = ClassicHfsAppendExtent(file->ext, file->fabn, end, vol->mdb.drNmAlBlks,
                                      blocks, updated);
  if (append == ClassicHfsCannotAppend) {
    v_freeblocks(vol, &blocks);
    ERROR(EIO, "inconsistent classic HFS fork extents at physical EOF");
    return -1;
  }
  if (append == ClassicHfsNewOverflowRecord) {
    unsigned char record[HFS_EXTRECMAXLEN];
    int reclen = ClassicHfsWriteExtentKey(record, file->fork, file->cat.u.fil.filFlNum, end);
    reclen += ClassicHfsEncodeExtents(record + reclen, updated);
    if (bt_insert(&vol->ext, record, reclen) < 0) {
      v_freeblocks(vol, &blocks);
      return -1;
    }
    file->fabn = end;
  } else {
      /* store the modified extent record */

      if (file->fabn)
	{
	  if ((n.nnum == 0 &&
	       v_extsearch(file, file->fabn, 0, &n) <= 0) ||
	      v_putextrec(&updated, &n) < 0)
	    {
	      v_freeblocks(vol, &blocks);
	      return -1;
	    }
	}
      else
	memcpy(extrec, updated, sizeof(updated));
    }
  memcpy(file->ext, updated, sizeof(updated));

  *pylen += blocks.xdrNumABlks * vol->mdb.drAlBlkSiz;

  file->flags |= HFS_UPDATE_CATREC;

  return blocks.xdrNumABlks;
}

/*
 * NAME:	file->trunc()
 * DESCRIPTION:	release disk blocks unneeded by a file
 */
int f_trunc(hfsfile *file)
{
  ExtDataRec *extrec;
  unsigned long *lglen, *pylen, alblksz;
  uint32_t newpylen;
  ClassicHfsForkFields forkFields = ClassicHfsSelectedFork(file);
  lglen = forkFields.logical;
  pylen = forkFields.physical;
  extrec = forkFields.extents;
  alblksz = file->vol->mdb.drAlBlkSiz;
  int sizeStatus = ClassicHfsTruncatedSize(*lglen, *pylen, alblksz, &newpylen);
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
  unsigned start = file->fabn <= keep ? file->fabn : 0;
  while (start < oldEnd) {
    ExtDataRec current;
    node n;
    if (start) {
      if (v_extsearch(file, start, &current, &n) <= 0)
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
        if (plan.released[i].xdrNumABlks && v_freeblocks(file->vol, &plan.released[i]) < 0)
          return -1;
      if (!start)
        memcpy(*extrec, plan.kept, sizeof(plan.kept));
      else if (plan.kept[0].xdrNumABlks) {
        if (v_putextrec(&plan.kept, &n) < 0)
          return -1;
      } else if (bt_delete(&file->vol->ext, HFS_NODEREC(n, n.rnum)) < 0)
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

/*
 * NAME:	file->flush()
 * DESCRIPTION:	flush all pending changes to an open file
 */
int f_flush(hfsfile *file)
{
  hfsvol *vol = file->vol;

  if (! (vol->flags & HFS_READONLY))
    {
      if (file->flags & HFS_UPDATE_CATREC)
	{
	  node n;

	  file->cat.u.fil.filStBlk   = file->cat.u.fil.filExtRec[0].xdrStABN;
	  file->cat.u.fil.filRStBlk  = file->cat.u.fil.filRExtRec[0].xdrStABN;
	  file->cat.u.fil.filClpSize = file->clump;

	  if (v_catsearch(file->vol, file->parid, file->name, 0, 0, &n) <= 0 ||
	      v_putcatrec(&file->cat, &n) < 0)
	    return -1;

	  file->flags &= ~HFS_UPDATE_CATREC;
	}
    }

  return 0;
}
