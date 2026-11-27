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
      if (ClassicHfsReadSector(vol, bnum, &b) < 0)
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
