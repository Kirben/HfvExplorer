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

/* Modified for emx by Marcus Better, July 1997 */

# include <string.h>
# include <stdlib.h>
# include <errno.h>

#ifdef WIN32
#define NODEVLOCKS
# include <io.h>
# include <stdio.h>
#else
# include <unistd.h>
#endif

# include <fcntl.h>

# include "internal.h"
# include "../ClassicHfsRecordBytes.h"
# include "block.h"
# include "low.h"
# include "file.h"

/*
 * NAME:	low->lockvol()
 * DESCRIPTION:	prevent destructive simultaneous access
 */
int l_lockvol(hfsvol *vol)
{
# ifndef NODEVLOCKS

  struct flock lock;

  lock.l_type   = (vol->flags & HFS_READONLY) ? F_RDLCK : F_WRLCK;
  lock.l_start  = 0;
  lock.l_whence = SEEK_SET;
  lock.l_len    = 0;

  if (fcntl(vol->fd, F_SETLK, &lock) < 0)
    {
      ERROR(errno, "unable to obtain lock for device");
      return -1;
    }

# endif

  return 0;
}

/*
 * NAME:	low->readblock0()
 * DESCRIPTION:	read the first sector and get bearings
 */
int l_readblock0(hfsvol *vol)
{
  block b;
  unsigned char *ptr = b;
  Block0 rec;

  if (b_readlb(vol, 0, &b) < 0)
    return -1;

  rec.sbSig = ClassicHfsTake16(&ptr);
  rec.sbBlkSize = ClassicHfsTake16(&ptr);
  rec.sbBlkCount = ClassicHfsTake32(&ptr);
  rec.sbDevType = ClassicHfsTake16(&ptr);
  rec.sbDevId = ClassicHfsTake16(&ptr);
  rec.sbData = ClassicHfsTake32(&ptr);
  rec.sbDrvrCount = ClassicHfsTake16(&ptr);
  rec.ddBlock = ClassicHfsTake32(&ptr);
  rec.ddSize = ClassicHfsTake16(&ptr);
  rec.ddType = ClassicHfsTake16(&ptr);

  switch (rec.sbSig)
    {
    case 0x4552:  /* block device with a partition table */
      {
	if (rec.sbBlkSize != HFS_BLOCKSZ)
	  {
	    ERROR(EINVAL, "unsupported block size");
	    return -1;
	  }

	vol->vlen = rec.sbBlkCount;

	if (l_readpm(vol) < 0)
	  return -1;
      }
      break;

    case 0x4c4b:  /* bootable floppy */
      vol->pnum = 0;
      break;

    default:  /* non-bootable floppy or something else */

      /* some miscreant media may also be partitioned;
	 we attempt to read a partition map, but ignore any failure */

      if (l_readpm(vol) < 0)
	vol->pnum = 0;
    }

  return 0;
}

/*
 * NAME:	low->readpm()
 * DESCRIPTION:	read the partition map and locate an HFS volume
 */
int l_readpm(hfsvol *vol)
{
  block b;
  unsigned char *ptr;
  Partition map;
  unsigned long bnum;
  int pnum;

  bnum = 1;
  pnum = vol->pnum;

  while (1)
    {
      if (b_readlb(vol, bnum, &b) < 0)
	return -1;

      ptr = b;

      map.pmSig = ClassicHfsTake16(&ptr);
      map.pmSigPad = ClassicHfsTake16(&ptr);
      map.pmMapBlkCnt = ClassicHfsTake32(&ptr);
      map.pmPyPartStart = ClassicHfsTake32(&ptr);
      map.pmPartBlkCnt = ClassicHfsTake32(&ptr);

      memcpy(map.pmPartName, ptr, 32);
      map.pmPartName[32] = 0;
      ptr += 32;

      memcpy(map.pmParType, ptr, 32);
      map.pmParType[32] = 0;
      ptr += 32;

      map.pmLgDataStart = ClassicHfsTake32(&ptr);
      map.pmDataCnt = ClassicHfsTake32(&ptr);
      map.pmPartStatus = ClassicHfsTake32(&ptr);
      map.pmLgBootStart = ClassicHfsTake32(&ptr);
      map.pmBootSize = ClassicHfsTake32(&ptr);
      map.pmBootAddr = ClassicHfsTake32(&ptr);
      map.pmBootAddr2 = ClassicHfsTake32(&ptr);
      map.pmBootEntry = ClassicHfsTake32(&ptr);
      map.pmBootEntry2 = ClassicHfsTake32(&ptr);
      map.pmBootCksum = ClassicHfsTake32(&ptr);

      memcpy(map.pmProcessor, ptr, 16);
      map.pmProcessor[16] = 0;
      ptr += 16;

      if (map.pmSig == 0x5453)
	{
	  /* old partition map sig */

	  ERROR(EINVAL, "unsupported partition map signature");
	  return -1;
	}

      if (map.pmSig != 0x504d)
	{
	  ERROR(EINVAL, "bad partition map");
	  return -1;
	}

      if (strcmp((char *) map.pmParType, "Apple_HFS") == 0 && --pnum == 0)
	{
	  if (map.pmLgDataStart != 0)
	    {
	      ERROR(EINVAL, "unsupported start of partition logical data");
	      return -1;
	    }

	  vol->vstart = map.pmPyPartStart;
	  vol->vlen   = map.pmPartBlkCnt;

	  return 0;
	}

      if (bnum >= map.pmMapBlkCnt)
	{
	  ERROR(EINVAL, "can't find HFS partition");
	  return -1;
	}

      ++bnum;
    }
}

/*
 * NAME:	low->readmdb()
 * DESCRIPTION:	read the master directory block into memory
 */
int l_readmdb(hfsvol *vol)
{
  block b;
  MDB *mdb = &vol->mdb;
  hfsfile *ext = &vol->ext.f;
  hfsfile *cat = &vol->cat.f;
  int i;

  if (b_readlb(vol, 2, &b) < 0)
    return -1;

  ClassicHfsDecodeMasterDirectory(b, mdb);

  vol->lpa = mdb->drAlBlkSiz >> HFS_BLOCKSZ_BITS;

  /* extents pseudo-file structs */

  ext->vol   = vol;
  ext->parid = 0;
  strcpy(ext->name, "extents overflow");

  ext->cat.cdrType          = cdrFilRec;
  /* ext->cat.cdrResrv2 */
  ext->cat.u.fil.filFlags   = 0;
  ext->cat.u.fil.filTyp     = 0;
  /* ext->cat.u.fil.filUsrWds */
  ext->cat.u.fil.filFlNum   = HFS_CNID_EXT;
  ext->cat.u.fil.filStBlk   = mdb->drXTExtRec[0].xdrStABN;
  ext->cat.u.fil.filLgLen   = mdb->drXTFlSize;
  ext->cat.u.fil.filPyLen   = mdb->drXTFlSize;
  ext->cat.u.fil.filRStBlk  = 0;
  ext->cat.u.fil.filRLgLen  = 0;
  ext->cat.u.fil.filRPyLen  = 0;
  ext->cat.u.fil.filCrDat   = mdb->drCrDate;
  ext->cat.u.fil.filMdDat   = mdb->drLsMod;
  ext->cat.u.fil.filBkDat   = 0;
  /* ext->cat.u.fil.filFndrInfo */
  ext->cat.u.fil.filClpSize = 0;

  memcpy(ext->cat.u.fil.filExtRec, mdb->drXTExtRec, sizeof(ExtDataRec));
  for (i = 0; i < 3; ++i)
    {
      ext->cat.u.fil.filRExtRec[i].xdrStABN    = 0;
      ext->cat.u.fil.filRExtRec[i].xdrNumABlks = 0;
    }
  f_selectfork(ext, 0);

  ext->clump = mdb->drXTClpSiz;
  ext->flags = 0;

  ext->prev = ext->next = 0;

  /* catalog pseudo-file structs */

  cat->vol   = vol;
  cat->parid = 0;
  strcpy(cat->name, "catalog");

  cat->cat.cdrType          = cdrFilRec;
  /* cat->cat.cdrResrv2 */
  cat->cat.u.fil.filFlags   = 0;
  cat->cat.u.fil.filTyp     = 0;
  /* cat->cat.u.fil.filUsrWds */
  cat->cat.u.fil.filFlNum   = HFS_CNID_CAT;
  cat->cat.u.fil.filStBlk   = mdb->drCTExtRec[0].xdrStABN;
  cat->cat.u.fil.filLgLen   = mdb->drCTFlSize;
  cat->cat.u.fil.filPyLen   = mdb->drCTFlSize;
  cat->cat.u.fil.filRStBlk  = 0;
  cat->cat.u.fil.filRLgLen  = 0;
  cat->cat.u.fil.filRPyLen  = 0;
  cat->cat.u.fil.filCrDat   = mdb->drCrDate;
  cat->cat.u.fil.filMdDat   = mdb->drLsMod;
  cat->cat.u.fil.filBkDat   = 0;
  /* cat->cat.u.fil.filFndrInfo */
  cat->cat.u.fil.filClpSize = 0;

  memcpy(cat->cat.u.fil.filExtRec, mdb->drCTExtRec, sizeof(ExtDataRec));
  for (i = 0; i < 3; ++i)
    {
      cat->cat.u.fil.filRExtRec[i].xdrStABN    = 0;
      cat->cat.u.fil.filRExtRec[i].xdrNumABlks = 0;
    }
  f_selectfork(cat, 0);

  cat->clump = mdb->drCTClpSiz;
  cat->flags = 0;

  cat->prev = cat->next = 0;

  return 0;
}

/*
 * NAME:	low->writemdb()
 * DESCRIPTION:	write the master directory block to disk
 */
int l_writemdb(hfsvol *vol)
{
  block b;
  MDB *mdb = &vol->mdb;
  hfsfile *ext = &vol->ext.f;
  hfsfile *cat = &vol->cat.f;

  memset(&b, 0, sizeof(b));

  mdb->drXTFlSize = ext->cat.u.fil.filPyLen;
  mdb->drXTClpSiz = ext->clump;
  memcpy(mdb->drXTExtRec, ext->cat.u.fil.filExtRec, sizeof(ExtDataRec));

  mdb->drCTFlSize = cat->cat.u.fil.filPyLen;
  mdb->drCTClpSiz = cat->clump;
  memcpy(mdb->drCTExtRec, cat->cat.u.fil.filExtRec, sizeof(ExtDataRec));

  ClassicHfsEncodeMasterDirectory(b, mdb);

  if (b_writelb(vol, 2, &b) < 0)
    return -1;

  if (vol->flags & HFS_UPDATE_ALTMDB)
    {
      if (b_writelb(vol, vol->vlen - 2, &b) < 0)
	return -1;
    }

  vol->flags &= ~(HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB);

  return 0;
}
