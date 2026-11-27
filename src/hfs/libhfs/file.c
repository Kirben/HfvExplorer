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
# include "block.h"
# include "file.h"
# include "btree.h"
# include "record.h"
# include "volume.h"

/*
 * NAME:	file->selectfork()
 * DESCRIPTION:	choose a fork for file operations
 */
void f_selectfork(hfsfile *file, int fork)
{
  if (fork == 0)
    {
      file->fork = fkData;
      memcpy(file->ext, file->cat.u.fil.filExtRec, sizeof(ExtDataRec));
    }
  else
    {
      file->fork = fkRsrc;
      memcpy(file->ext, file->cat.u.fil.filRExtRec, sizeof(ExtDataRec));
    }

  file->fabn = 0;
  file->pos  = 0;
}

/*
 * NAME:	file->getptrs()
 * DESCRIPTION:	make pointers to the current fork's lengths and extents
 */
void f_getptrs(hfsfile *file, unsigned long **lglen,
	       unsigned long **pylen, ExtDataRec **extrec)
{
  if (file->fork == fkData)
    {
      if (lglen)
	*lglen  = &file->cat.u.fil.filLgLen;
      if (pylen)
	*pylen  = &file->cat.u.fil.filPyLen;
      if (extrec)
	*extrec = &file->cat.u.fil.filExtRec;
    }
  else
    {
      if (lglen)
	*lglen  = &file->cat.u.fil.filRLgLen;
      if (pylen)
	*pylen  = &file->cat.u.fil.filRPyLen;
      if (extrec)
	*extrec = &file->cat.u.fil.filRExtRec;
    }
}

/*
 * NAME:	file->doblock()
 * DESCRIPTION:	read or write a numbered block from a file
 */
int f_doblock(hfsfile *file, unsigned long num, block *bp,
	       int (*func)(hfsvol *, unsigned int, unsigned int, block *))
{
  unsigned int abnum;
  unsigned int blnum;
  unsigned int fabn;
  int i;

  abnum = num / file->vol->lpa;
  blnum = num % file->vol->lpa;

  /* locate the appropriate extent record */

  fabn = file->fabn;

  if (abnum < fabn)
    {
      ExtDataRec *extrec;

      f_getptrs(file, 0, 0, &extrec);

      fabn = file->fabn = 0;
      memcpy(file->ext, extrec, sizeof(ExtDataRec));
    }
  else
    abnum -= fabn;

  while (1)
    {
      unsigned int num;

      for (i = 0; i < 3; ++i)
	{
	  num = file->ext[i].xdrNumABlks;

	  if (abnum < num)
	    return func(file->vol, file->ext[i].xdrStABN + abnum, blnum, bp);

	  fabn  += num;
	  abnum -= num;
	}

      if (fabn <= file->fabn) { ERROR(EIO, "empty extent record"); return -1; }
      if (v_extsearch(file, fabn, &file->ext, 0) <= 0)
	return -1;

      file->fabn = fabn;
    }
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

  f_getptrs(file, 0, &pylen, 0);
  /* Physical EOF is also a 32-bit on-disk field. Never wrap at 4 GiB. */
  {
    unsigned long available = (0xffffffffUL - *pylen) / vol->mdb.drAlBlkSiz;
    unsigned long request = clumpsz / vol->mdb.drAlBlkSiz;
    if (!available) { ERROR(EFBIG, "classic HFS physical fork size limit"); return -1; }
    if (!request) request = 1;
    if (request > available) request = available;
    if (request > 65535UL) request = 65535UL;
    blocks.xdrNumABlks = (unsigned short)request;
  }

  if (v_allocblocks(vol, &blocks) < 0)
    return -1;

  /* update the file's extents */

  f_getptrs(file, 0, &pylen, &extrec);

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

  if (i >= 0 &&
      file->ext[i].xdrStABN + file->ext[i].xdrNumABlks == blocks.xdrStABN)
    file->ext[i].xdrNumABlks += blocks.xdrNumABlks;
  else
    {
      /* create a new extent descriptor */

      if (++i < 3)
	file->ext[i] = blocks;
      else
	{
	  unsigned char record[HFS_EXTRECMAXLEN];
	  int reclen;

	  /* record is full; create a new one */

	  file->ext[0] = blocks;

	  for (i = 1; i < 3; ++i)
	    {
	      file->ext[i].xdrStABN    = 0;
	      file->ext[i].xdrNumABlks = 0;
	    }

	  file->fabn = start;

	  reclen = ClassicHfsWriteExtentKey(record, file->fork, file->cat.u.fil.filFlNum, end);
	  reclen += ClassicHfsEncodeExtents(record + reclen, file->ext);

	  if (bt_insert(&vol->ext, record, reclen) < 0)
	    {
	      v_freeblocks(vol, &blocks);
	      return -1;
	    }

	  i = -1;
	}
    }

  if (i >= 0)
    {
      /* store the modified extent record */

      if (file->fabn)
	{
	  if ((n.nnum == 0 &&
	       v_extsearch(file, file->fabn, 0, &n) <= 0) ||
	      v_putextrec(&file->ext, &n) < 0)
	    {
	      v_freeblocks(vol, &blocks);
	      return -1;
	    }
	}
      else
	memcpy(extrec, file->ext, sizeof(ExtDataRec));
    }

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
  unsigned long *lglen, *pylen, alblksz, newpylen;
  unsigned int dlen, start, end;
  node n;
  int i;

  f_getptrs(file, &lglen, &pylen, &extrec);

  alblksz  = file->vol->mdb.drAlBlkSiz;
  {
    hfv_size_t rounded = ((hfv_size_t)*lglen + alblksz - 1) / alblksz * alblksz;
    if (rounded > 0xffffffffULL) { ERROR(EFBIG, "classic HFS physical fork size limit"); return -1; }
    newpylen = (unsigned long)rounded;
  }

  if (newpylen > *pylen)
    {
      ERROR(EIO, "file size exceeds physical length");
      return -1;
    }
  else if (newpylen == *pylen)
    return 0;

  dlen  = (*pylen - newpylen) / alblksz;

  start = file->fabn;
  end   = newpylen / alblksz;

  if (start >= end)
    {
      start = file->fabn = 0;
      memcpy(file->ext, extrec, sizeof(ExtDataRec));
    }

  n.nnum = 0;
  i      = -1;

  while (start < end)
    {
      for (i = 0; i < 3; ++i)
	{
	  unsigned int num;

	  num    = file->ext[i].xdrNumABlks;
	  start += num;

	  if (start >= end)
	    break;
	  else if (num == 0)
	    {
	      ERROR(EIO, "empty file extent");
	      return -1;
	    }
	}

      if (start >= end)
	break;

      if (v_extsearch(file, start, &file->ext, &n) <= 0)
	return -1;

      file->fabn = start;
    }

  if (start > end)
    {
      ExtDescriptor blocks;

      unsigned int release = start - end;
      unsigned int kept = file->ext[i].xdrNumABlks - release;
      unsigned int first = file->ext[i].xdrStABN + kept;
      if (release > dlen || first > UINT16_MAX) {
          ERROR(EIO, "invalid classic HFS fork tail");
          return -1;
      }
      blocks.xdrStABN = (uint16_t)first;
      blocks.xdrNumABlks = (uint16_t)release;
      if (v_freeblocks(file->vol, &blocks) < 0)
          return -1;
      file->ext[i].xdrNumABlks = (uint16_t)kept;
      dlen -= release;
    }

  *pylen = newpylen;

  file->flags |= HFS_UPDATE_CATREC;

  do
    {
      while (dlen && ++i < 3)
	{
	  unsigned int num;

	  num    = file->ext[i].xdrNumABlks;
	  start += num;

	  if (num == 0)
	    {
	      ERROR(EIO, "empty file extent");
	      return -1;
	    }
	  else if (num > dlen)
	    {
	      ERROR(EIO, "file extents exceed physical size");
	      return -1;
	    }

	  if (v_freeblocks(file->vol, &file->ext[i]) < 0)
	    return -1;
	  dlen -= num;

	  file->ext[i].xdrStABN    = 0;
	  file->ext[i].xdrNumABlks = 0;
	}

      if (file->fabn)
	{
	  if (n.nnum == 0 &&
	      v_extsearch(file, file->fabn, 0, &n) <= 0)
	    return -1;

	  if (file->ext[0].xdrNumABlks)
	    {
	      if (v_putextrec(&file->ext, &n) < 0)
		return -1;
	    }
	  else
	    {
	      if (bt_delete(&file->vol->ext, HFS_NODEREC(n, n.rnum)) < 0)
		return -1;

	      n.nnum = 0;
	    }
	}
      else
	memcpy(extrec, file->ext, sizeof(ExtDataRec));

      if (dlen)
	{
	  if (v_extsearch(file, start, &file->ext, &n) <= 0)
	    return -1;

	  file->fabn = start;
	  i = -1;
	}
    }
  while (dlen);

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
