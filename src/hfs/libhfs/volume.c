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

# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <time.h>

# include "../ClassicHfsNames.h"
# include "internal.h"
#include "../ClassicHfsCatalogLeaf.h"
#include "../ClassicHfsPath.h"
#include "../ClassicHfsTime.h"
#include "../ClassicHfsVolumeBitmap.h"
# include "btree.h"
#include "../ClassicHfsTreeFlush.h"
# include "volume.h"

/*
 * NAME:	vol->destruct()
 * DESCRIPTION:	free memory consumed by a volume descriptor
 */
void v_destruct(hfsvol *vol)
{
  FREE(vol->failure_message);
  FREE(vol->vbm);

  FREE(vol->ext.map);
  FREE(vol->cat.map);

  FREE(vol);
}

/*
 * NAME:	vol->getvol()
 * DESCRIPTION:	validate a volume reference
 */
int v_getvol(hfsvol **vol)
{
  if (*vol == 0)
    {
      if (hfs_curvol == 0)
	{
	  ERROR(EINVAL, "no volume is current");
	  return -1;
	}

      *vol = hfs_curvol;
    }

  return 0;
}

/*
 * NAME:	vol->flush()
 * DESCRIPTION:	flush all pending changes (B*-tree, MDB, VBM) to disk
 */
int v_flush(hfsvol *vol, int umounting)
{
  if (! (vol->flags & HFS_READONLY))
    {
      if ((vol->ext.flags & HFS_UPDATE_BTHDR) &&
	  ClassicHfsFlushTreeHeader(&vol->ext) < 0)
	return -1;

      if ((vol->cat.flags & HFS_UPDATE_BTHDR) &&
	  ClassicHfsFlushTreeHeader(&vol->cat) < 0)
	return -1;

      if ((vol->flags & HFS_UPDATE_VBM) &&
	  ClassicHfsFlushVolumeBitmap(vol) < 0)
	return -1;

      if (umounting &&
	  ! (vol->mdb.drAtrb & HFS_ATRB_UMOUNTED))
	{
	  vol->mdb.drAtrb |= HFS_ATRB_UMOUNTED;
	  vol->flags |= HFS_UPDATE_MDB;
	}

      if ((vol->flags & (HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB)) &&
	  ClassicHfsStoreVolumeHeader(vol) < 0)
	return -1;
    }

  return 0;
}
