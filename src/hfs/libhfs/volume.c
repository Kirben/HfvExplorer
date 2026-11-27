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

/*
 * NAME:	vol->newfolder()
 * DESCRIPTION:	create a new HFS folder
 */
int v_newfolder(hfsvol *vol, long parid, char *name)
{
  CatDataRec data;
  long id;
  unsigned char record[HFS_CATRECMAXLEN];
  int i, reclen;

  if (ClassicHfsReserveTreeSpace(&vol->cat, 2) < 0)
    return -1;

  id = vol->mdb.drNxtCNID++;
  vol->flags |= HFS_UPDATE_MDB;

  /* create directory record */

  data.cdrType   = cdrDirRec;
  data.cdrResrv2 = 0;

  data.u.dir.dirFlags = 0;
  data.u.dir.dirVal   = 0;
  data.u.dir.dirDirID = id;
  data.u.dir.dirCrDat = ClassicHfsFromUnixSeconds(time(0));
  data.u.dir.dirMdDat = data.u.dir.dirCrDat;
  data.u.dir.dirBkDat = 0;

  memset(&data.u.dir.dirUsrInfo,  0, sizeof(data.u.dir.dirUsrInfo));
  memset(&data.u.dir.dirFndrInfo, 0, sizeof(data.u.dir.dirFndrInfo));
  for (i = 0; i < 4; ++i)
    data.u.dir.dirResrv[i] = 0;

  if ((reclen = ClassicHfsWriteCatalogKey(record, parid, name)) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }
  {
    unsigned size = ClassicHfsEncodeCatalogRecord(record + reclen, sizeof(record) - reclen, &data);
    if (!size) { ERROR(EIO, "invalid classic HFS catalog record size"); return -1; }
    reclen += size;
  }

  if (ClassicHfsInsertTreeRecord(&vol->cat, record, reclen) < 0)
    return -1;

  /* create thread record */

  data.cdrType   = cdrThdRec;
  data.cdrResrv2 = 0;

  data.u.dthd.thdResrv[0] = 0;
  data.u.dthd.thdResrv[1] = 0;
  data.u.dthd.thdParID    = parid;
  strcpy(data.u.dthd.thdCName, name);

  if ((reclen = ClassicHfsWriteCatalogKey(record, id, "")) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }
  {
    unsigned size = ClassicHfsEncodeCatalogRecord(record + reclen, sizeof(record) - reclen, &data);
    if (!size) { ERROR(EIO, "invalid classic HFS catalog record size"); return -1; }
    reclen += size;
  }

  if (ClassicHfsInsertTreeRecord(&vol->cat, record, reclen) < 0 ||
      ClassicHfsAdjustEntryCount(vol, parid, 1, 1) < 0)
    return -1;

  return 0;
}

/* Check every descriptor before touching the allocation bitmap. */
int v_recovery_extents_valid(const ExtDataRec *extents, unsigned int blocks)
{
  unsigned int i;
  for (i = 0; i < 3; ++i)
    {
      unsigned int start = (*extents)[i].xdrStABN;
      unsigned int count = (*extents)[i].xdrNumABlks;

      if (count && (start >= blocks || count > blocks - start))
        return 0;
    }
  return 1;
}

static int markexts(hfsvol *vol, const ExtDataRec *extents)
{
  unsigned int i;
  if (!v_recovery_extents_valid(extents, vol->mdb.drNmAlBlks))
    {
      ERROR(EIO, "extent outside volume allocation bitmap during HFS recovery");
      return -1;
    }

  for (i = 0; i < 3; ++i)
    {
      unsigned int start = (*extents)[i].xdrStABN;
      unsigned int count = (*extents)[i].xdrNumABlks;
      unsigned int index;
      for (index = start; index < start + count; ++index)
        ClassicHfsSetBit(vol->vbm, index);
    }
  return 0;
}

int v_recovery_leaf_valid(const node *leaf, unsigned long previous)
{
  /* The first leaf must link back to zero; each later leaf must link back
     to its immediate predecessor. A repeated leaf cannot satisfy this. */
  return (unsigned char)leaf->nd.ndType == ndLeafNode &&
         leaf->nd.ndNHeight == 1 && leaf->nd.ndBLink == previous;
}

static int scavenge_tree(hfsvol *vol, btree *tree, int catalog)
{
  unsigned long previous = 0;
  node leaf;
  leaf.bt = tree;

  for (leaf.nnum = tree->hdr.bthFNode; leaf.nnum; leaf.nnum = leaf.nd.ndFLink)
    {
      unsigned int i, record_limit;
      if (leaf.nnum >= tree->hdr.bthNNodes || leaf.nnum / 8 >= tree->mapsz)
        {
          ERROR(EIO, "invalid B-tree leaf number during HFS recovery");
          return -1;
        }
      if (ClassicHfsReadTreeNode(&leaf) < 0)
        return -1;
      if (!v_recovery_leaf_valid(&leaf, previous))
        {
          ERROR(EIO, "cyclic or broken B-tree leaf chain during HFS recovery");
          return -1;
        }
      record_limit = HFS_BLOCKSZ - 2 * (leaf.nd.ndNRecs + 1);
      for (i = 0; i < leaf.nd.ndNRecs; ++i)
        {
          unsigned int start = leaf.roff[i], end = leaf.roff[i + 1], key_size;
          unsigned char *record;
          if (start < 14 || start >= end || end > record_limit)
            goto invalid_record;
          key_size = HFS_RECKEYSKIP(leaf.data + start);
          if (key_size >= end - start)
            goto invalid_record;
          record = leaf.data + start + key_size;
          if (catalog)
            {
              if (record[0] == cdrFilRec)
                {
                  CatDataRec data;
                  if (end - start - key_size < HFS_CATDATALEN)
                    goto invalid_record;
                  ClassicHfsDecodeCatalogRecord(record, end - start - key_size, &data);
                  if (markexts(vol, &data.u.fil.filExtRec) < 0 ||
                      markexts(vol, &data.u.fil.filRExtRec) < 0)
                    return -1;
                }
            }
          else
            {
              ExtDataRec data;
              if (end - start - key_size < HFS_EXTDATALEN)
                goto invalid_record;
              ClassicHfsDecodeExtents(record, data);
              if (markexts(vol, &data) < 0)
                return -1;
            }
        }
      previous = leaf.nnum;
    }
  if (previous != tree->hdr.bthLNode)
    {
      ERROR(EIO, "incomplete B-tree leaf chain during HFS recovery");
      return -1;
    }
  return 0;

invalid_record:
  ERROR(EIO, "invalid B-tree leaf record during HFS recovery");
  return -1;
}

/*
 * NAME:	vol->scavenge()
 * DESCRIPTION:	safeguard blocks in the volume bitmap
 */
int v_scavenge(hfsvol *vol)
{
  unsigned int pt, blks;
  if (vol->vbm == 0)
    return 0;

  if (markexts(vol, &vol->mdb.drXTExtRec) < 0 ||
      markexts(vol, &vol->mdb.drCTExtRec) < 0 ||
      scavenge_tree(vol, &vol->ext, 0) < 0 ||
      scavenge_tree(vol, &vol->cat, 1) < 0)
    return -1;

  vol->flags |= HFS_UPDATE_VBM;
  for (blks = 0, pt = vol->mdb.drNmAlBlks; pt--; )
    if (! ClassicHfsBitIsSet(vol->vbm, pt))
      ++blks;

  if (vol->mdb.drFreeBks != blks)
    {
      vol->mdb.drFreeBks = blks;
      vol->flags |= HFS_UPDATE_MDB;
    }

  return 0;
}
