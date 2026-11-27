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
# include "data.h"
# include "../ClassicHfsRecordBytes.h"
# include "record.h"

/*
 * NAME:	record->packcatkey()
 * DESCRIPTION:	pack a catalog record key
 */
void r_packcatkey(CatKeyRec *key, unsigned char *pkey, int *len)
{
  unsigned char *start = pkey;

  ClassicHfsAppend8(&pkey, key->ckrKeyLen);
  ClassicHfsAppend8(&pkey, key->ckrResrv1);
  ClassicHfsAppend32(&pkey, key->ckrParID);
  ClassicHfsAppendName(&pkey, key->ckrCName, sizeof(key->ckrCName));

  if (len)
    *len = HFS_RECKEYSKIP(start);
}

/*
 * NAME:	record->unpackcatkey()
 * DESCRIPTION:	unpack a catalog record key
 */
void r_unpackcatkey(unsigned char *pkey, CatKeyRec *key)
{
  key->ckrKeyLen = ClassicHfsTake8(&pkey);
  key->ckrResrv1 = ClassicHfsTake8(&pkey);
  key->ckrParID = ClassicHfsTake32(&pkey);
  ClassicHfsTakeName(&pkey, key->ckrCName, sizeof(key->ckrCName));
}

/*
 * NAME:	record->packextkey()
 * DESCRIPTION:	pack an extents record key
 */
void r_packextkey(ExtKeyRec *key, unsigned char *pkey, int *len)
{
  unsigned char *start = pkey;

  ClassicHfsAppend8(&pkey, key->xkrKeyLen);
  ClassicHfsAppend8(&pkey, key->xkrFkType);
  ClassicHfsAppend32(&pkey, key->xkrFNum);
  ClassicHfsAppend16(&pkey, key->xkrFABN);

  if (len)
    *len = HFS_RECKEYSKIP(start);
}

/*
 * NAME:	record->unpackextkey()
 * DESCRIPTION:	unpack an extents record key
 */
void r_unpackextkey(unsigned char *pkey, ExtKeyRec *key)
{
  key->xkrKeyLen = ClassicHfsTake8(&pkey);
  key->xkrFkType = ClassicHfsTake8(&pkey);
  key->xkrFNum = ClassicHfsTake32(&pkey);
  key->xkrFABN = ClassicHfsTake16(&pkey);
}

/*
 * NAME:	record->comparecatkeys()
 * DESCRIPTION:	compare two (packed) catalog record keys
 */
int r_comparecatkeys(unsigned char *pkey1, unsigned char *pkey2)
{
  CatKeyRec key1;
  CatKeyRec key2;
  int diff;

  r_unpackcatkey(pkey1, &key1);
  r_unpackcatkey(pkey2, &key2);

  diff = key1.ckrParID - key2.ckrParID;
  if (diff)
    return diff;

  return ClassicHfsCompareNames(key1.ckrCName, key2.ckrCName);
}

/*
 * NAME:	record->compareextkeys()
 * DESCRIPTION:	compare two (packed) extents record keys
 */
int r_compareextkeys(unsigned char *pkey1, unsigned char *pkey2)
{
  ExtKeyRec key1;
  ExtKeyRec key2;
  int diff;

  r_unpackextkey(pkey1, &key1);
  r_unpackextkey(pkey2, &key2);

  diff = key1.xkrFNum - key2.xkrFNum;
  if (diff)
    return diff;

  diff = (unsigned char) key1.xkrFkType -
         (unsigned char) key2.xkrFkType;
  if (diff)
    return diff;

  return key1.xkrFABN - key2.xkrFABN;
}

/*
 * NAME:	record->packcatdata()
 * DESCRIPTION:	pack catalog record data
 */
void r_packcatdata(CatDataRec *data, unsigned char *pdata, int *len)
{
  unsigned char *start = pdata;
  int i;

  ClassicHfsAppend8(&pdata, data->cdrType);
  ClassicHfsAppend8(&pdata, data->cdrResrv2);

  switch (data->cdrType)
    {
    case cdrDirRec:
      ClassicHfsAppend16(&pdata, data->u.dir.dirFlags);
      ClassicHfsAppend16(&pdata, data->u.dir.dirVal);
      ClassicHfsAppend32(&pdata, data->u.dir.dirDirID);
      ClassicHfsAppend32(&pdata, data->u.dir.dirCrDat);
      ClassicHfsAppend32(&pdata, data->u.dir.dirMdDat);
      ClassicHfsAppend32(&pdata, data->u.dir.dirBkDat);

      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frRect.top);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frRect.left);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frRect.bottom);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frRect.right);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frFlags);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frLocation.v);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frLocation.h);
      ClassicHfsAppend16(&pdata, data->u.dir.dirUsrInfo.frView);

      ClassicHfsAppend16(&pdata, data->u.dir.dirFndrInfo.frScroll.v);
      ClassicHfsAppend16(&pdata, data->u.dir.dirFndrInfo.frScroll.h);
      ClassicHfsAppend32(&pdata, data->u.dir.dirFndrInfo.frOpenChain);
      ClassicHfsAppend16(&pdata, data->u.dir.dirFndrInfo.frUnused);
      ClassicHfsAppend16(&pdata, data->u.dir.dirFndrInfo.frComment);
      ClassicHfsAppend32(&pdata, data->u.dir.dirFndrInfo.frPutAway);

      for (i = 0; i < 4; ++i)
	ClassicHfsAppend32(&pdata, data->u.dir.dirResrv[i]);

      break;

    case cdrFilRec:
      ClassicHfsAppend8(&pdata, data->u.fil.filFlags);
      ClassicHfsAppend8(&pdata, data->u.fil.filTyp);

      ClassicHfsAppend32(&pdata, data->u.fil.filUsrWds.fdType);
      ClassicHfsAppend32(&pdata, data->u.fil.filUsrWds.fdCreator);
      ClassicHfsAppend16(&pdata, data->u.fil.filUsrWds.fdFlags);
      ClassicHfsAppend16(&pdata, data->u.fil.filUsrWds.fdLocation.v);
      ClassicHfsAppend16(&pdata, data->u.fil.filUsrWds.fdLocation.h);
      ClassicHfsAppend16(&pdata, data->u.fil.filUsrWds.fdFldr);

      ClassicHfsAppend32(&pdata, data->u.fil.filFlNum);

      ClassicHfsAppend16(&pdata, data->u.fil.filStBlk);
      ClassicHfsAppend32(&pdata, data->u.fil.filLgLen);
      ClassicHfsAppend32(&pdata, data->u.fil.filPyLen);

      ClassicHfsAppend16(&pdata, data->u.fil.filRStBlk);
      ClassicHfsAppend32(&pdata, data->u.fil.filRLgLen);
      ClassicHfsAppend32(&pdata, data->u.fil.filRPyLen);

      ClassicHfsAppend32(&pdata, data->u.fil.filCrDat);
      ClassicHfsAppend32(&pdata, data->u.fil.filMdDat);
      ClassicHfsAppend32(&pdata, data->u.fil.filBkDat);

      ClassicHfsAppend16(&pdata, data->u.fil.filFndrInfo.fdIconID);
      for (i = 0; i < 4; ++i)
	ClassicHfsAppend16(&pdata, data->u.fil.filFndrInfo.fdUnused[i]);
      ClassicHfsAppend16(&pdata, data->u.fil.filFndrInfo.fdComment);
      ClassicHfsAppend32(&pdata, data->u.fil.filFndrInfo.fdPutAway);

      ClassicHfsAppend16(&pdata, data->u.fil.filClpSize);

      for (i = 0; i < 3; ++i)
	{
	  ClassicHfsAppend16(&pdata, data->u.fil.filExtRec[i].xdrStABN);
	  ClassicHfsAppend16(&pdata, data->u.fil.filExtRec[i].xdrNumABlks);
	}

      for (i = 0; i < 3; ++i)
	{
	  ClassicHfsAppend16(&pdata, data->u.fil.filRExtRec[i].xdrStABN);
	  ClassicHfsAppend16(&pdata, data->u.fil.filRExtRec[i].xdrNumABlks);
	}

      ClassicHfsAppend32(&pdata, data->u.fil.filResrv);
      break;

    case cdrThdRec:
      for (i = 0; i < 2; ++i)
	ClassicHfsAppend32(&pdata, data->u.dthd.thdResrv[i]);

      ClassicHfsAppend32(&pdata, data->u.dthd.thdParID);
      ClassicHfsAppendName(&pdata, data->u.dthd.thdCName, sizeof(data->u.dthd.thdCName));
      break;

    case cdrFThdRec:
      for (i = 0; i < 2; ++i)
	ClassicHfsAppend32(&pdata, data->u.fthd.fthdResrv[i]);

      ClassicHfsAppend32(&pdata, data->u.fthd.fthdParID);
      ClassicHfsAppendName(&pdata, data->u.fthd.fthdCName, sizeof(data->u.fthd.fthdCName));
      break;

# ifdef DEBUG
    default:
      abort();
# endif
    }

  if (len)
    *len += pdata - start;
}

/*
 * NAME:	record->unpackcatdata()
 * DESCRIPTION:	unpack catalog record data
 */
void r_unpackcatdata(unsigned char *pdata, CatDataRec *data)
{
  int i;

  data->cdrType = ClassicHfsTake8(&pdata);
  data->cdrResrv2 = ClassicHfsTake8(&pdata);

  switch (data->cdrType)
    {
    case cdrDirRec:
      data->u.dir.dirFlags = ClassicHfsTake16(&pdata);
      data->u.dir.dirVal = ClassicHfsTake16(&pdata);
      data->u.dir.dirDirID = ClassicHfsTake32(&pdata);
      data->u.dir.dirCrDat = ClassicHfsTake32(&pdata);
      data->u.dir.dirMdDat = ClassicHfsTake32(&pdata);
      data->u.dir.dirBkDat = ClassicHfsTake32(&pdata);

      data->u.dir.dirUsrInfo.frRect.top = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frRect.left = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frRect.bottom = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frRect.right = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frFlags = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frLocation.v = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frLocation.h = ClassicHfsTake16(&pdata);
      data->u.dir.dirUsrInfo.frView = ClassicHfsTake16(&pdata);

      data->u.dir.dirFndrInfo.frScroll.v = ClassicHfsTake16(&pdata);
      data->u.dir.dirFndrInfo.frScroll.h = ClassicHfsTake16(&pdata);
      data->u.dir.dirFndrInfo.frOpenChain = ClassicHfsTake32(&pdata);
      data->u.dir.dirFndrInfo.frUnused = ClassicHfsTake16(&pdata);
      data->u.dir.dirFndrInfo.frComment = ClassicHfsTake16(&pdata);
      data->u.dir.dirFndrInfo.frPutAway = ClassicHfsTake32(&pdata);

      for (i = 0; i < 4; ++i)
	data->u.dir.dirResrv[i] = ClassicHfsTake32(&pdata);

      break;

    case cdrFilRec:
      data->u.fil.filFlags = ClassicHfsTake8(&pdata);
      data->u.fil.filTyp = ClassicHfsTake8(&pdata);

      data->u.fil.filUsrWds.fdType = ClassicHfsTake32(&pdata);
      data->u.fil.filUsrWds.fdCreator = ClassicHfsTake32(&pdata);
      data->u.fil.filUsrWds.fdFlags = ClassicHfsTake16(&pdata);
      data->u.fil.filUsrWds.fdLocation.v = ClassicHfsTake16(&pdata);
      data->u.fil.filUsrWds.fdLocation.h = ClassicHfsTake16(&pdata);
      data->u.fil.filUsrWds.fdFldr = ClassicHfsTake16(&pdata);

      data->u.fil.filFlNum = ClassicHfsTake32(&pdata);

      data->u.fil.filStBlk = ClassicHfsTake16(&pdata);
      data->u.fil.filLgLen = ClassicHfsTake32(&pdata);
      data->u.fil.filPyLen = ClassicHfsTake32(&pdata);

      data->u.fil.filRStBlk = ClassicHfsTake16(&pdata);
      data->u.fil.filRLgLen = ClassicHfsTake32(&pdata);
      data->u.fil.filRPyLen = ClassicHfsTake32(&pdata);

      data->u.fil.filCrDat = ClassicHfsTake32(&pdata);
      data->u.fil.filMdDat = ClassicHfsTake32(&pdata);
      data->u.fil.filBkDat = ClassicHfsTake32(&pdata);

      data->u.fil.filFndrInfo.fdIconID = ClassicHfsTake16(&pdata);
      for (i = 0; i < 4; ++i)
	data->u.fil.filFndrInfo.fdUnused[i] = ClassicHfsTake16(&pdata);
      data->u.fil.filFndrInfo.fdComment = ClassicHfsTake16(&pdata);
      data->u.fil.filFndrInfo.fdPutAway = ClassicHfsTake32(&pdata);

      data->u.fil.filClpSize = ClassicHfsTake16(&pdata);

      for (i = 0; i < 3; ++i)
	{
	  data->u.fil.filExtRec[i].xdrStABN = ClassicHfsTake16(&pdata);
	  data->u.fil.filExtRec[i].xdrNumABlks = ClassicHfsTake16(&pdata);
	}

      for (i = 0; i < 3; ++i)
	{
	  data->u.fil.filRExtRec[i].xdrStABN = ClassicHfsTake16(&pdata);
	  data->u.fil.filRExtRec[i].xdrNumABlks = ClassicHfsTake16(&pdata);
	}

      data->u.fil.filResrv = ClassicHfsTake32(&pdata);
      break;

    case cdrThdRec:
      for (i = 0; i < 2; ++i)
	data->u.dthd.thdResrv[i] = ClassicHfsTake32(&pdata);

      data->u.dthd.thdParID = ClassicHfsTake32(&pdata);
      ClassicHfsTakeName(&pdata, data->u.dthd.thdCName, sizeof(data->u.dthd.thdCName));
      break;

    case cdrFThdRec:
      for (i = 0; i < 2; ++i)
	data->u.fthd.fthdResrv[i] = ClassicHfsTake32(&pdata);

      data->u.fthd.fthdParID = ClassicHfsTake32(&pdata);
      ClassicHfsTakeName(&pdata, data->u.fthd.fthdCName, sizeof(data->u.fthd.fthdCName));
      break;

# ifdef DEBUG
    default:
      abort();
# endif
    }
}

/*
 * NAME:	record->packextdata()
 * DESCRIPTION:	pack extent record data
 */
void r_packextdata(ExtDataRec *data, unsigned char *pdata, int *len)
{
  unsigned char *start = pdata;
  int i;

  for (i = 0; i < 3; ++i)
    {
      ClassicHfsAppend16(&pdata, (*data)[i].xdrStABN);
      ClassicHfsAppend16(&pdata, (*data)[i].xdrNumABlks);
    }

  if (len)
    *len += pdata - start;
}

/*
 * NAME:	record->unpackextdata()
 * DESCRIPTION:	unpack extent record data
 */
void r_unpackextdata(unsigned char *pdata, ExtDataRec *data)
{
  int i;

  for (i = 0; i < 3; ++i)
    {
      (*data)[i].xdrStABN = ClassicHfsTake16(&pdata);
      (*data)[i].xdrNumABlks = ClassicHfsTake16(&pdata);
    }
}

/*
 * NAME:	record->makecatkey()
 * DESCRIPTION:	construct a catalog record key
 */
void r_makecatkey(CatKeyRec *key, long parid, char *name)
{
  int len;

  len = strlen(name) + 1;

  key->ckrKeyLen = 0x05 + len + (len & 1);
  key->ckrResrv1 = 0;
  key->ckrParID  = parid;

  strcpy(key->ckrCName, name);
}

/*
 * NAME:	record->makeextkey()
 * DESCRIPTION:	construct an extents record key
 */
void r_makeextkey(ExtKeyRec *key, int fork, long fnum, unsigned int fabn)
{
  key->xkrKeyLen = 0x07;
  key->xkrFkType = fork;
  key->xkrFNum   = fnum;
  key->xkrFABN   = fabn;
}

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

      ent->crdate = d_toutime(data->u.dir.dirCrDat);
      ent->mddate = d_toutime(data->u.dir.dirMdDat);
      ent->bkdate = d_toutime(data->u.dir.dirBkDat);

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

      ent->crdate = d_toutime(data->u.fil.filCrDat);
      ent->mddate = d_toutime(data->u.fil.filMdDat);
      ent->bkdate = d_toutime(data->u.fil.filBkDat);

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
      data->u.dir.dirCrDat = d_tomtime(ent->crdate);
      data->u.dir.dirMdDat = d_tomtime(ent->mddate);
      data->u.dir.dirBkDat = d_tomtime(ent->bkdate);

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

      data->u.fil.filCrDat = d_tomtime(ent->crdate);
      data->u.fil.filMdDat = d_tomtime(ent->mddate);
      data->u.fil.filBkDat = d_tomtime(ent->bkdate);

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
