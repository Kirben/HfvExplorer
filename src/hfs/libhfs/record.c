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

# include "../ClassicHfsNames.h"
# include "internal.h"
#include "../ClassicHfsTime.h"
#include "../ClassicHfsBytes.h"
# include "../ClassicHfsRecordBytes.h"
# include "record.h"

/*
 * NAME:	record->unpackdirent()
 * DESCRIPTION:	unpack catalog information into hfsdirent structure
 */
void r_unpackdirent(long parid, char *name, CatDataRec *data, hfsdirent *ent)
{
  strcpy(ent->name, name);
  ent->parid = parid;

  switch (data->cdrType)
    {
    case cdrDirRec:
      ent->flags = HFS_ISDIR;
      ent->cnid  = data->u.dir.dirDirID;

      ent->crdate = ClassicHfsToUnixSeconds(data->u.dir.dirCrDat);
      ent->mddate = ClassicHfsToUnixSeconds(data->u.dir.dirMdDat);
      ent->bkdate = ClassicHfsToUnixSeconds(data->u.dir.dirBkDat);

      ent->fdflags      = data->u.dir.dirUsrInfo.frFlags;
      ent->fdlocation.v = data->u.dir.dirUsrInfo.frLocation.v;
      ent->fdlocation.h = data->u.dir.dirUsrInfo.frLocation.h;

      ent->u.dir.valence = data->u.dir.dirVal;

      ent->u.dir.rect.top    = data->u.dir.dirUsrInfo.frRect.top;
      ent->u.dir.rect.left   = data->u.dir.dirUsrInfo.frRect.left;
      ent->u.dir.rect.bottom = data->u.dir.dirUsrInfo.frRect.bottom;
      ent->u.dir.rect.right  = data->u.dir.dirUsrInfo.frRect.right;

      break;

    case cdrFilRec:
      ent->flags = (data->u.fil.filFlags & (1 << 0)) ? HFS_ISLOCKED : 0;
      ent->cnid  = data->u.fil.filFlNum;

      ent->crdate = ClassicHfsToUnixSeconds(data->u.fil.filCrDat);
      ent->mddate = ClassicHfsToUnixSeconds(data->u.fil.filMdDat);
      ent->bkdate = ClassicHfsToUnixSeconds(data->u.fil.filBkDat);

      ent->fdflags      = data->u.fil.filUsrWds.fdFlags;
      ent->fdlocation.v = data->u.fil.filUsrWds.fdLocation.v;
      ent->fdlocation.h = data->u.fil.filUsrWds.fdLocation.h;

      ent->u.file.dsize = data->u.fil.filLgLen;
      ent->u.file.rsize = data->u.fil.filRLgLen;

      ClassicHfsWrite32((unsigned char *) ent->u.file.type,
	     data->u.fil.filUsrWds.fdType);
      ClassicHfsWrite32((unsigned char *) ent->u.file.creator,
	     data->u.fil.filUsrWds.fdCreator);

      ent->u.file.type[4] = ent->u.file.creator[4] = 0;

      break;
    }
}

/*
 * NAME:	record->packdirent()
 * DESCRIPTION:	make changes to a catalog record
 */
void r_packdirent(CatDataRec *data, hfsdirent *ent)
{
  switch (data->cdrType)
    {
    case cdrDirRec:
      data->u.dir.dirCrDat = ClassicHfsFromUnixSeconds(ent->crdate);
      data->u.dir.dirMdDat = ClassicHfsFromUnixSeconds(ent->mddate);
      data->u.dir.dirBkDat = ClassicHfsFromUnixSeconds(ent->bkdate);

      data->u.dir.dirUsrInfo.frFlags      = ent->fdflags;
      data->u.dir.dirUsrInfo.frLocation.v = ent->fdlocation.v;
      data->u.dir.dirUsrInfo.frLocation.h = ent->fdlocation.h;

      data->u.dir.dirUsrInfo.frRect.top    = ent->u.dir.rect.top;
      data->u.dir.dirUsrInfo.frRect.left   = ent->u.dir.rect.left;
      data->u.dir.dirUsrInfo.frRect.bottom = ent->u.dir.rect.bottom;
      data->u.dir.dirUsrInfo.frRect.right  = ent->u.dir.rect.right;

      break;

    case cdrFilRec:
      if (ent->flags & HFS_ISLOCKED)
	data->u.fil.filFlags |=  (1 << 0);
      else
	data->u.fil.filFlags &= ~(1 << 0);

      data->u.fil.filCrDat = ClassicHfsFromUnixSeconds(ent->crdate);
      data->u.fil.filMdDat = ClassicHfsFromUnixSeconds(ent->mddate);
      data->u.fil.filBkDat = ClassicHfsFromUnixSeconds(ent->bkdate);

      data->u.fil.filUsrWds.fdFlags      = ent->fdflags;
      data->u.fil.filUsrWds.fdLocation.v = ent->fdlocation.v;
      data->u.fil.filUsrWds.fdLocation.h = ent->fdlocation.h;

      data->u.fil.filUsrWds.fdType =
	ClassicHfsRead32((unsigned char *) ent->u.file.type);
      data->u.fil.filUsrWds.fdCreator =
	ClassicHfsRead32((unsigned char *) ent->u.file.creator);

      break;
    }
}
