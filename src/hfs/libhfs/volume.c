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
#include "../ClassicHfsTime.h"
# include "low.h"
# include "btree.h"
# include "record.h"
# include "volume.h"

static HFV_THREAD_LOCAL int m_invalid_cd_hack = 0;

void set_invalid_cd_hack( int onoff )
{
	m_invalid_cd_hack = onoff;
}

/*
 * NAME:	vol->catsearch()
 * DESCRIPTION:	search catalog tree
 */
int v_catsearch(hfsvol *vol, long parid, char *name,
		CatDataRec *data, char *cname, node *np)
{
  CatKeyRec key;
  unsigned char pkey[HFS_CATKEYLEN];
  node n;
  unsigned char *ptr;
  int found;

  if (np == 0)
    np = &n;

  if (ClassicHfsWriteCatalogKey(pkey, parid, name) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }

  found = bt_search(&vol->cat, pkey, np);
  if (found <= 0)
    return found;

  ptr = HFS_NODEREC(*np, np->rnum);

  if (cname)
    {
      if (!ClassicHfsReadCatalogKey(ptr, np->roff[np->rnum + 1] - np->roff[np->rnum], &key)) {
        ERROR(EIO, "invalid catalog key");
        return -1;
      }
      strcpy(cname, key.ckrCName);
    }

  if (data)
    r_unpackcatdata(HFS_RECDATA(ptr), data);

  return 1;
}

/*
 * NAME:	vol->extsearch()
 * DESCRIPTION:	search extents tree
 */
int v_extsearch(hfsfile *file, unsigned int fabn, ExtDataRec *data, node *np)
{
  ExtDataRec extsave;
  unsigned int fabnsave;
  unsigned char pkey[HFS_EXTKEYLEN];
  node n;
  unsigned char *ptr;
  int found;

  if (np == 0)
    np = &n;

  ClassicHfsWriteExtentKey(pkey, file->fork, file->cat.u.fil.filFlNum, fabn);

  /* in case bt_search() clobbers these */

  memcpy(&extsave, &file->ext, sizeof(ExtDataRec));
  fabnsave = file->fabn;

  if (file->cat.u.fil.filFlNum == 3)
    {
      /* The root can itself be beyond the inline extents. Bootstrap the
         overflow file from readable leaves instead of recursing via its root. */
      unsigned long previous = 0, visits = 0;
      if (file->flags & HFS_EXT_BOOTSTRAP) {
        ERROR(EIO, "recursive extents-file bootstrap");
        return -1;
      }
      file->flags |= HFS_EXT_BOOTSTRAP;
      np->bt = &file->vol->ext;
      np->nnum = np->bt->hdr.bthFNode;
      found = 0;
      while (np->nnum) {
        if (++visits > np->bt->hdr.bthNNodes) {
          ERROR(EIO, "cycle in classic HFS overflow leaf chain");
          found = -1;
          break;
        }
        if (bt_getnode(np) < 0) { found = -1; break; }
        if (np->nd.ndType != -1 || np->nd.ndNHeight != 1 || np->nd.ndBLink != previous) {
          ERROR(EIO, "invalid classic HFS overflow leaf link");
          found = -1;
          break;
        }
        found = ClassicHfsScanExtentLeaf(np->data, np->roff, np->nd.ndNRecs, pkey, &np->rnum);
        if (found == ClassicHfsBadExtentLeaf)
          { ERROR(EIO, "invalid classic HFS overflow leaf record"); }
        if (found != ClassicHfsContinueExtentScan)
          break;
        previous = np->nnum;
        np->nnum = np->nd.ndFLink;
        found = 0;
      }
      file->flags &= ~HFS_EXT_BOOTSTRAP;
    }
  else
    found = bt_search(&file->vol->ext, pkey, np);


  memcpy(&file->ext, &extsave, sizeof(ExtDataRec));
  file->fabn = fabnsave;

  if (found <= 0)
    return found;

  if (data)
    {
      ptr = HFS_NODEREC(*np, np->rnum);
      if (!ClassicHfsValidExtentRecord(ptr, np->roff[np->rnum + 1] - np->roff[np->rnum])) {
          ERROR(EIO, "invalid overflow extent record");
          return -1;
      }
      ClassicHfsDecodeExtents(ptr + ClassicHfsExtentKeySize, *data);
    }

  return 1;
}

/*
 * NAME:	vol->getthread()
 * DESCRIPTION:	retrieve catalog thread information for a file or directory
 */
int v_getthread(hfsvol *vol, long id, CatDataRec *thread, node *np, int type)
{
  CatDataRec rec;
  int found;

  if (thread == 0)
    thread = &rec;

  found = v_catsearch(vol, id, "", thread, 0, np);
  if (found <= 0)
    return found;

  if (thread->cdrType != type)
    {
      ERROR(EIO, "bad thread record");
      return -1;
    }

  return 1;
}

/*
 * NAME:	vol->putcatrec()
 * DESCRIPTION:	store catalog information
 */
int v_putcatrec(CatDataRec *data, node *np)
{
  unsigned char pdata[HFS_CATDATALEN], *ptr;
  int len = 0;

  r_packcatdata(data, pdata, &len);

  ptr = HFS_NODEREC(*np, np->rnum);
  memcpy(HFS_RECDATA(ptr), pdata, len);

  return bt_putnode(np);
}

/*
 * NAME:	vol->putextrec()
 * DESCRIPTION:	store extent information
 */
int v_putextrec(ExtDataRec *data, node *np)
{
  unsigned char *ptr = HFS_NODEREC(*np, np->rnum);
  if (!ClassicHfsValidExtentRecord(ptr, np->roff[np->rnum + 1] - np->roff[np->rnum])) {
      ERROR(EIO, "invalid overflow extent record");
      return -1;
  }
  ClassicHfsEncodeExtents(ptr + ClassicHfsExtentKeySize, *data);

  return bt_putnode(np);
}

/*
 * NAME:	vol->allocblocks()
 * DESCRIPTION:	allocate a contiguous range of blocks
 */
int v_allocblocks(hfsvol *vol, ExtDescriptor *blocks)
{
  unsigned int found;

  if (vol->mdb.drFreeBks == 0)
    {
      ERROR(ENOSPC, "volume full");
      return -1;
    }

  found = ClassicHfsAllocateBlocks(vol->vbm, vol->mdb.drNmAlBlks,
                                   &vol->mdb.drAllocPtr, &vol->mdb.drFreeBks,
                                   blocks->xdrNumABlks, &blocks->xdrStABN);
  if (!found)
    {
      ERROR(EIO, "bad volume bitmap or free block count");
      return -1;
    }
  blocks->xdrNumABlks = (uint16_t)found;

  vol->flags |= HFS_UPDATE_MDB | HFS_UPDATE_VBM;

  return 0;
}

/*
 * NAME:	vol->freeblocks()
 * DESCRIPTION:	deallocate a contiguous range of blocks
 */
int v_freeblocks(hfsvol *vol, const ExtDescriptor *blocks)
{
  if (!ClassicHfsReleaseBlocks(vol->vbm, vol->mdb.drNmAlBlks, blocks->xdrStABN,
                              blocks->xdrNumABlks, &vol->mdb.drFreeBks)) {
      ERROR(EIO, "invalid classic HFS extent release");
      return -1;
  }
  vol->flags |= HFS_UPDATE_MDB | HFS_UPDATE_VBM;
  return 0;
}

/*
 * NAME:	vol->resolve()
 * DESCRIPTION:	translate a pathname; return catalog information
 */
int v_resolve(hfsvol *vol, char *path, CatDataRec *data,
	      long *parid, char *fname, node *np)
{
  long dirid;
  char name[HFS_MAX_FLEN + 1], *nptr;
  int found;

  if (*path == 0)
    {
      ERROR(ENOENT, "empty path");
      return -1;
    }

  if (parid)
    *parid = 0;

	// Hack by lp to tolerate invalid CD's.
	// Delete the volume name and always use aprtial names.

	if(m_invalid_cd_hack) {
		if(*path != ':') {
			char *p = strchr( path, ':' );
			if(p) {
				path = p;
			}
		}
	}

  nptr = strchr(path, ':');

  if (*path == ':' || nptr == 0)
    {
      dirid = vol->cwd;  /* relative path */

      if (*path == ':')
	++path;

      if (*path == 0)
	{
	  found = v_getdthread(vol, dirid, data, 0);
	  if (found <= 0)
	    return found;

	  if (parid)
	    *parid = data->u.dthd.thdParID;

	  return v_catsearch(vol, data->u.dthd.thdParID,
			     data->u.dthd.thdCName, data, fname, np);
	}
    }
  else
    {
      dirid = HFS_CNID_ROOTPAR;  /* absolute path */

      if (nptr - path > HFS_MAX_VLEN)
	{
	  ERROR(ENAMETOOLONG, 0);
	  return -1;
	}

      strncpy(name, path, nptr - path);
      name[nptr - path] = 0;

      /* The caller already selected the mount. A label only confirms it. */
      if (ClassicHfsCompareNames(vol->mdb.drVN, name) != 0)
        {
          ERROR(ENOENT, "path does not name the selected HFS volume");
          return -1;
        }
    }

  while (1)
    {
      while (*path == ':')
	{
	  ++path;

	  found = v_getdthread(vol, dirid, data, 0);
	  if (found <= 0)
	    return found;

	  dirid = data->u.dthd.thdParID;
	}

      if (*path == 0)
	{
	  found = v_getdthread(vol, dirid, data, 0);
	  if (found <= 0)
	    return found;

	  if (parid)
	    *parid = data->u.dthd.thdParID;

	  return v_catsearch(vol, data->u.dthd.thdParID,
			     data->u.dthd.thdCName, data, fname, np);
	}

      nptr = name;
      while (nptr < name + sizeof(name) - 1 && *path && *path != ':')
	*nptr++ = *path++;

      if (*path && *path != ':')
	{
	  ERROR(ENAMETOOLONG, 0);
	  return -1;
	}

      *nptr = 0;
      if (*path == ':')
	++path;

      if (parid)
	*parid = dirid;

      found = v_catsearch(vol, dirid, name, data, fname, np);
      if (found < 0)
	return -1;

      if (found == 0)
	{
	  if (*path && parid)
	    *parid = 0;

	  if (*path == 0 && fname)
	    strcpy(fname, name);

	  return 0;
	}

      switch (data->cdrType)
	{
	case cdrDirRec:
	  if (*path == 0)
	    return 1;

	  dirid = data->u.dir.dirDirID;
	  break;

	case cdrFilRec:
	  if (*path == 0)
	    return 1;

	  ERROR(ENOTDIR, "invalid pathname");
	  return -1;

	default:
	  ERROR(EIO, "unexpected catalog record");
	  return -1;
	}
    }
}

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
	  bt_writehdr(&vol->ext) < 0)
	return -1;

      if ((vol->cat.flags & HFS_UPDATE_BTHDR) &&
	  bt_writehdr(&vol->cat) < 0)
	return -1;

      if ((vol->flags & HFS_UPDATE_VBM) &&
	  l_writevbm(vol) < 0)
	return -1;

      if (umounting &&
	  ! (vol->mdb.drAtrb & HFS_ATRB_UMOUNTED))
	{
	  vol->mdb.drAtrb |= HFS_ATRB_UMOUNTED;
	  vol->flags |= HFS_UPDATE_MDB;
	}

      if ((vol->flags & (HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB)) &&
	  l_writemdb(vol) < 0)
	return -1;
    }

  return 0;
}

/*
 * NAME:	vol->adjvalence()
 * DESCRIPTION:	update a volume's valence counts
 */
int v_adjvalence(hfsvol *vol, long parid, int isdir, int adj)
{
  node n;
  CatDataRec data;

  if (isdir)
    vol->mdb.drDirCnt += adj;
  else
    vol->mdb.drFilCnt += adj;

  vol->flags |= HFS_UPDATE_MDB;

  if (parid == HFS_CNID_ROOTDIR)
    {
      if (isdir)
	vol->mdb.drNmRtDirs += adj;
      else
	vol->mdb.drNmFls    += adj;
    }
  else if (parid == HFS_CNID_ROOTPAR)
    return 0;

  if (v_getdthread(vol, parid, &data, 0) <= 0 ||
      v_catsearch(vol, data.u.dthd.thdParID, data.u.dthd.thdCName,
		  &data, 0, &n) <= 0 ||
      data.cdrType != cdrDirRec)
    {
      ERROR(EIO, "can't find parent directory");
      return -1;
    }

  data.u.dir.dirVal  += adj;
  data.u.dir.dirMdDat = ClassicHfsFromUnixSeconds(time(0));

  return v_putcatrec(&data, &n);
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

  if (bt_space(&vol->cat, 2) < 0)
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
  r_packcatdata(&data, HFS_RECDATA(record), &reclen);

  if (bt_insert(&vol->cat, record, reclen) < 0)
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
  r_packcatdata(&data, HFS_RECDATA(record), &reclen);

  if (bt_insert(&vol->cat, record, reclen) < 0 ||
      v_adjvalence(vol, parid, 1, 1) < 0)
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
      if (bt_getnode(&leaf) < 0)
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
                  r_unpackcatdata(record, &data);
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
