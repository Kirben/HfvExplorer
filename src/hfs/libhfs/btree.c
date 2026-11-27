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

# include "internal.h"
#include "../ClassicHfsBytes.h"
# include "block.h"
# include "file.h"
# include "btree.h"
# include "node.h"

/*
 * NAME:	btree->getnode()
 * DESCRIPTION:	retrieve a numbered node from a B*-tree file
 */
int bt_getnode(node *np)
{
  btree *bt = np->bt;
  block *bp = &np->data;

  /* verify the node exists and is marked as in-use */

  if (np->nnum < 0 || (np->nnum > 0 && np->nnum >= bt->hdr.bthNNodes))
    {
      ERROR(EIO, "read nonexistent b*-tree node");
      return -1;
    }

  if (bt->map && (np->nnum / 8 >= bt->mapsz || !ClassicHfsBitIsSet(bt->map, np->nnum)))
    {
      ERROR(EIO, "read unallocated b*-tree node");
      return -1;
    }

  if (f_getblock(&bt->f, np->nnum, bp) < 0)
    return -1;

  if (!ClassicHfsDecodeNode(*bp, &np->nd, np->roff)) {
      ERROR(EIO, "invalid b*-tree node record offsets");
      return -1;
  }

  return 0;
}

/*
 * NAME:	btree->putnode()
 * DESCRIPTION:	store a numbered node into a B*-tree file
 */
int bt_putnode(node *np)
{
  btree *bt = np->bt;
  block *bp = &np->data;

  /* verify the node exists and is marked as in-use */

  if (np->nnum && np->nnum >= bt->hdr.bthNNodes)
    {
      ERROR(EIO, "write nonexistent b*-tree node");
      return -1;
    }
  else if (bt->map && ! ClassicHfsBitIsSet(bt->map, np->nnum))
    {
      ERROR(EIO, "write unallocated b*-tree node");
      return -1;
    }

  if (!ClassicHfsEncodeNode(*bp, &np->nd, np->roff)) {
      ERROR(EIO, "invalid b*-tree node record offsets");
      return -1;
  }

  if (f_putblock(&bt->f, np->nnum, bp) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	btree->readhdr()
 * DESCRIPTION:	read the header node of a B*-tree
 */
int bt_readhdr(btree *bt)
{
  unsigned char *ptr;
  char *map;
  unsigned long nnum;

  bt->hdrnd.bt   = bt;
  bt->hdrnd.nnum = 0;

  if (bt_getnode(&bt->hdrnd) < 0)
    return -1;

  if (bt->hdrnd.nd.ndType != ndHdrNode ||
      bt->hdrnd.nd.ndNRecs != 3 ||
      bt->hdrnd.roff[0] != 0x00e ||
      bt->hdrnd.roff[1] != 0x078 ||
      bt->hdrnd.roff[2] != 0x0f8 ||
      bt->hdrnd.roff[3] != 0x1f8)
    {
      ERROR(EIO, "malformed b*-tree header node");
      return -1;
    }

  /* read header record */

  ptr = HFS_NODEREC(bt->hdrnd, 0);

  if (!ClassicHfsDecodeTreeHeader(ptr, bt->hdrnd.roff[1] - bt->hdrnd.roff[0], &bt->hdr)) {
      ERROR(EIO, "truncated b*-tree header record");
      return -1;
  }

  if (bt->hdr.bthNodeSize != HFS_BLOCKSZ)
    {
      ERROR(EINVAL, "unsupported b*-tree node size");
      return -1;
    }

  /* read map record; construct btree bitmap */
  /* don't set bt->map until we're done, since getnode() checks it */

  map = ALLOC(char, HFS_MAP1SZ);
  if (map == 0)
    {
      ERROR(ENOMEM, 0);
      return -1;
    }

  memcpy(map, HFS_NODEREC(bt->hdrnd, 2), HFS_MAP1SZ);
  bt->mapsz = HFS_MAP1SZ;

  /* read continuation map records, if any */

  nnum = bt->hdrnd.nd.ndFLink;

  while (nnum)
    {
      node n;
      char *newmap;

      n.bt   = bt;
      n.nnum = nnum;

      if (bt_getnode(&n) < 0)
	{
	  FREE(map);
	  return -1;
	}

      if (n.nd.ndType != ndMapNode ||
	  n.nd.ndNRecs != 1 ||
	  n.roff[0] != 0x00e ||
	  n.roff[1] != 0x1fa)
	{
	  FREE(map);
	  ERROR(EIO, "malformed b*-tree map node");
	  return -1;
	}

      newmap = REALLOC(map, char, bt->mapsz + HFS_MAPXSZ);
      if (newmap == 0)
	{
	  FREE(map);
	  ERROR(ENOMEM, 0);
	  return -1;
	}
      map = newmap;

      memcpy(map + bt->mapsz, HFS_NODEREC(n, 0), HFS_MAPXSZ);
      bt->mapsz += HFS_MAPXSZ;

      nnum = n.nd.ndFLink;
    }

  bt->map = map;

  return 0;
}

/*
 * NAME:	btree->writehdr()
 * DESCRIPTION:	write the header node of a B*-tree
 */
int bt_writehdr(btree *bt)
{
  unsigned char *ptr;
  char *map;
  unsigned long mapsz, nnum;

# ifdef DEBUG
  if (bt->hdrnd.bt != bt ||
      bt->hdrnd.nnum != 0 ||
      bt->hdrnd.nd.ndType != ndHdrNode ||
      bt->hdrnd.nd.ndNRecs != 3)
    abort();
# endif

  ptr = HFS_NODEREC(bt->hdrnd, 0);

  if (!ClassicHfsEncodeTreeHeader(ptr, bt->hdrnd.roff[1] - bt->hdrnd.roff[0], &bt->hdr)) {
      ERROR(EIO, "truncated b*-tree header record");
      return -1;
  }

  memcpy(HFS_NODEREC(bt->hdrnd, 2), bt->map, HFS_MAP1SZ);

  if (bt_putnode(&bt->hdrnd) < 0)
    return -1;

  map   = bt->map   + HFS_MAP1SZ;
  mapsz = bt->mapsz - HFS_MAP1SZ;

  nnum  = bt->hdrnd.nd.ndFLink;

  while (mapsz)
    {
      node n;

      if (nnum == 0)
	{
	  ERROR(EIO, "truncated b*-tree map");
	  return -1;
	}

      n.bt   = bt;
      n.nnum = nnum;

      if (bt_getnode(&n) < 0)
	return -1;

      if (n.nd.ndType != ndMapNode ||
	  n.nd.ndNRecs != 1 ||
	  n.roff[0] != 0x00e ||
	  n.roff[1] != 0x1fa)
	{
	  ERROR(EIO, "malformed b*-tree map node");
	  return -1;
	}

      memcpy(HFS_NODEREC(n, 0), map, HFS_MAPXSZ);

      if (bt_putnode(&n) < 0)
	return -1;

      map   += HFS_MAPXSZ;
      mapsz -= HFS_MAPXSZ;

      nnum = n.nd.ndFLink;
    }

  bt->flags &= ~HFS_UPDATE_BTHDR;

  return 0;
}

/* High-Level B*-Tree Routines ============================================= */

/*
 * NAME:	btree->space()
 * DESCRIPTION:	assert space for new records, or extend the file
 */
int bt_space(btree *bt, unsigned int nrecs)
{
  unsigned int nnodes;
  int space;

  nnodes = nrecs * (bt->hdr.bthDepth + 1);

  if (nnodes <= bt->hdr.bthFree)
    return 0;

  /* make sure the extents tree has room too */

  if (bt != &bt->f.vol->ext)
    {
      if (bt_space(&bt->f.vol->ext, 1) < 0)
	return -1;
    }

  space = f_alloc(&bt->f);
  if (space < 0)
    return -1;

  nnodes = space * (bt->f.vol->mdb.drAlBlkSiz / bt->hdr.bthNodeSize);

  bt->hdr.bthNNodes += nnodes;
  bt->hdr.bthFree   += nnodes;

  bt->flags |= HFS_UPDATE_BTHDR;

  bt->f.vol->flags |= HFS_UPDATE_ALTMDB;

  while (bt->hdr.bthNNodes > bt->mapsz * 8)
    {
      char *newmap;
      node mapnd;

      /* extend tree map */

      newmap = REALLOC(bt->map, char, bt->mapsz + HFS_MAPXSZ);
      if (newmap == 0)
	{
	  ERROR(ENOMEM, 0);
	  return -1;
	}

      memset(newmap + bt->mapsz, 0, HFS_MAPXSZ);

      bt->map    = newmap;
      bt->mapsz += HFS_MAPXSZ;

      n_init(&mapnd, bt, ndMapNode, 0);
      if (n_new(&mapnd) < 0)
	return -1;

      /* link the new map node */

      if (bt->hdrnd.nd.ndFLink == 0)
	{
	  bt->hdrnd.nd.ndFLink = mapnd.nnum;
	  mapnd.nd.ndBLink     = 0;
	}
      else
	{
	  node n;

	  n.bt   = bt;
	  n.nnum = bt->hdrnd.nd.ndFLink;

	  while (1)
	    {
	      if (bt_getnode(&n) < 0)
		return -1;

	      if (n.nd.ndFLink == 0)
		break;

	      n.nnum = n.nd.ndFLink;
	    }

	  n.nd.ndFLink     = mapnd.nnum;
	  mapnd.nd.ndBLink = n.nnum;

	  if (bt_putnode(&n) < 0)
	    return -1;
	}

      mapnd.nd.ndNRecs = 1;
      mapnd.roff[1]    = 0x1fa;

      if (bt_putnode(&mapnd) < 0)
	return -1;
    }

  return 0;
}

/*
 * NAME:	btree->insertx()
 * DESCRIPTION:	recursively locate a node and insert a record
 */
static int bt_insertx(node *np, unsigned height, unsigned char *record, int *reclen)
{
  node child;
  unsigned char *rec;

  uint32_t next;
  int route = ClassicHfsLookupLevel(np->data, &np->nd, np->roff, height,
                                    np->bt == &np->bt->f.vol->cat, record, np->bt->compare,
                                    &np->rnum, np->bt->hdr.bthNNodes, &next, ClassicHfsInsert);
  if (route == ClassicHfsInvalidSearch) {
    ERROR(EIO, "invalid classic HFS B-tree mutation path");
    return -1;
  }
  if (route == ClassicHfsFoundKey)
    {
      ERROR(EIO, "b*-tree record already exists");
      return -1;
    }

  switch ((unsigned char) np->nd.ndType)
    {
    case ndIndxNode:
      if (np->rnum < 0)
	rec = HFS_NODEREC(*np, 0);
      else
	rec = HFS_NODEREC(*np, np->rnum);

      child.bt   = np->bt;
      child.nnum = next;

      if (bt_getnode(&child) < 0 ||
	  bt_insertx(&child, height - 1, record, reclen) < 0)
	return -1;

      if (np->rnum < 0)
	{
	  if (n_index(&child, rec, 0) < 0)
	    return -1;
	  np->rnum = 0; /* the split separator follows the updated first child */
	  if (*reclen == 0)
	    return bt_putnode(np);
	}

      return *reclen ? n_insert(np, record, reclen) : 0;

    case ndLeafNode:
      return n_insert(np, record, reclen);

    default:
      ERROR(EIO, "unexpected b*-tree node");
      return -1;
    }
}

/*
 * NAME:	btree->insert()
 * DESCRIPTION:	insert a new node record into a tree
 */
int bt_insert(btree *bt, unsigned char *record, int reclen)
{
  node root;

  if (bt->hdr.bthRoot == 0)
    {
      /* create root node */

      n_init(&root, bt, ndLeafNode, 1);
      if (n_new(&root) < 0 ||
	  bt_putnode(&root) < 0)
	return -1;

      bt->hdr.bthDepth = 1;
      bt->hdr.bthRoot  = root.nnum;
      bt->hdr.bthFNode = root.nnum;
      bt->hdr.bthLNode = root.nnum;

      bt->flags |= HFS_UPDATE_BTHDR;
    }
  else
    {
      root.bt   = bt;
      root.nnum = bt->hdr.bthRoot;

      if (bt_getnode(&root) < 0)
	return -1;
    }

  if (bt_insertx(&root, bt->hdr.bthDepth, record, &reclen) < 0)
    return -1;

  if (reclen)
    {
      unsigned char oroot[HFS_MAXRECLEN];
      int orootlen;

      /* root node was split; create a new root */

      if (n_index(&root, oroot, &orootlen) < 0)
        return -1;

      n_init(&root, bt, ndIndxNode, root.nd.ndNHeight + 1);
      if (n_new(&root) < 0)
	return -1;

      ++bt->hdr.bthDepth;
      bt->hdr.bthRoot = root.nnum;

      bt->flags |= HFS_UPDATE_BTHDR;

      /* insert index records for new root */

      n_search(&root, oroot);
      if (n_insertx(&root, oroot, orootlen) < 0)
        return -1;

      n_search(&root, record);
      if (n_insertx(&root, record, reclen) < 0)
        return -1;

      if (bt_putnode(&root) < 0)
	return -1;
    }

  ++bt->hdr.bthNRecs;
  bt->flags |= HFS_UPDATE_BTHDR;

  return 0;
}

/*
 * NAME:	btree->deletex()
 * DESCRIPTION:	recursively locate a node and delete a record
 */
static int bt_deletex(node *np, unsigned height, unsigned char *key, unsigned char *record, int *flag)
{
  node child;
  unsigned char *rec;
  uint32_t next;
  int route = ClassicHfsLookupLevel(np->data, &np->nd, np->roff, height,
                                    np->bt == &np->bt->f.vol->cat, key, np->bt->compare,
                                    &np->rnum, np->bt->hdr.bthNNodes, &next, ClassicHfsDelete);
  if (route == ClassicHfsInvalidSearch) {
    ERROR(EIO, "invalid classic HFS B-tree mutation path");
    return -1;
  }

  switch ((unsigned char) np->nd.ndType)
    {
    case ndIndxNode:
      if (np->rnum < 0)
	{
	  ERROR(EIO, "b*-tree record not found");
	  return -1;
	}

      rec = HFS_NODEREC(*np, np->rnum);

      child.bt   = np->bt;
      child.nnum = next;

      if (bt_getnode(&child) < 0 ||
	  bt_deletex(&child, height - 1, key, rec, flag) < 0)
	return -1;

      if (*flag)
	{
	  *flag = 0;

	  if (HFS_RECKEYLEN(rec) == 0)
	    return n_delete(np, record, flag);

	  if (np->rnum == 0)
	    {
	      if (n_index(np, record, 0) < 0)
	        return -1;
	      *flag = 1;
	    }

	  return bt_putnode(np);
	}

      return 0;

    case ndLeafNode:
      if (route != ClassicHfsFoundKey)
	{
	  ERROR(EIO, "b*-tree record not found");
	  return -1;
	}

      return n_delete(np, record, flag);

    default:
      ERROR(EIO, "unexpected b*-tree node");
      return -1;
    }
}

/*
 * NAME:	btree->delete()
 * DESCRIPTION:	remove a node record from a tree
 */
int bt_delete(btree *bt, unsigned char *key)
{
  node root;
  unsigned char record[HFS_MAXRECLEN];
  int flag = 0;

  root.bt   = bt;
  root.nnum = bt->hdr.bthRoot;

  if (root.nnum == 0)
    {
      ERROR(EIO, "empty b*-tree");
      return -1;
    }

  if (bt_getnode(&root) < 0 ||
      bt_deletex(&root, bt->hdr.bthDepth, key, record, &flag) < 0)
    return -1;

  if (bt->hdr.bthDepth > 1 && root.nd.ndNRecs == 1)
    {
      unsigned char *rec;

      /* chop the root */

      rec = HFS_NODEREC(root, 0);

      if (n_free(&root) < 0)
        return -1;
      --bt->hdr.bthDepth;
      bt->hdr.bthRoot = ClassicHfsRead32(HFS_RECDATA(rec));
    }
  else if (bt->hdr.bthDepth == 1 && root.nd.ndNRecs == 0)
    {
      /* delete the root node */

      if (n_free(&root) < 0)
        return -1;
      bt->hdr.bthDepth = 0;
      bt->hdr.bthRoot  = 0;
      bt->hdr.bthFNode = 0;
      bt->hdr.bthLNode = 0;
    }

  --bt->hdr.bthNRecs;
  bt->flags |= HFS_UPDATE_BTHDR;

  return 0;
}

/*
 * NAME:	btree->search()
 * DESCRIPTION:	locate a data record given a search key
 */
int bt_search(btree *bt, unsigned char *key, node *np)
{
  np->bt   = bt;
  np->nnum = bt->hdr.bthRoot;

  if (np->nnum == 0)
    {
      ERROR(ENOENT, 0);
      return 0;
    }

  unsigned height = bt->hdr.bthDepth;
  for (;;) {
    uint32_t child;
    if (bt_getnode(np) < 0)
      return -1;
    int result = ClassicHfsLookupLevel(np->data, &np->nd, np->roff, height,
                                       bt == &bt->f.vol->cat, key, bt->compare,
                                       &np->rnum, bt->hdr.bthNNodes, &child, ClassicHfsLookup);
    if (result == ClassicHfsInvalidSearch) {
      ERROR(EIO, "invalid classic HFS B-tree search path");
      return -1;
    }
    if (result == ClassicHfsMissingKey) {
      ERROR(ENOENT, 0);
      return 0;
    }
    if (result == ClassicHfsFoundKey)
      return 1;
    np->nnum = child;
    --height;
  }
}
