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
#include "../ClassicHfsNodeIO.h"
# include "block.h"
# include "file.h"
# include "btree.h"
# include "node.h"
#include "../ClassicHfsGrowTreeMap.h"

static int read_node_block(void *file, uint32_t number, block *bytes)
{
  return f_getblock((hfsfile *)file, number, bytes);
}

static int write_node_block(void *file, uint32_t number, block *bytes)
{
  return f_putblock((hfsfile *)file, number, bytes);
}

static int transfer_node(node *np, int writing)
{
  btree *bt = np->bt;
  ClassicHfsNodeStatus result = ClassicHfsTransferNode(np->nnum, bt->hdr.bthNNodes,
      bt->map, bt->mapsz, &np->data, &np->nd, np->roff, writing,
      writing ? write_node_block : read_node_block, &bt->f);
  if (result == ClassicHfsNodeUnavailable)
    { ERROR(EIO, "classic HFS B-tree node outside allocated map"); }
  else if (result == ClassicHfsNodeBadOffsets)
    { ERROR(EIO, "invalid classic HFS B-tree node offsets"); }
  return result == ClassicHfsNodeOk ? 0 : -1;
}

int bt_getnode(node *np)
{
  return transfer_node(np, 0);
}

int bt_putnode(node *np)
{
  return transfer_node(np, 1);
}

/* High-Level B*-Tree Routines ============================================= */

// Reserve enough free nodes for a split at every level. Growing either
// tree's node bitmap can consume some of the nodes just allocated.
int ClassicHfsReserveTreeSpace(btree *tree, unsigned records)
{
    uint64_t required = (uint64_t)records * (tree->hdr.bthDepth + 1u);
    if (required > UINT32_MAX / ClassicHfsNodeSize) {
        ERROR(EFBIG, "classic HFS tree reservation exceeds fork capacity");
        return -1;
    }
    hfsvol *volume = tree->f.vol;
    unsigned blockBytes = volume->mdb.drAlBlkSiz;
    if (tree->hdr.bthNodeSize != ClassicHfsNodeSize || !blockBytes ||
        blockBytes % ClassicHfsNodeSize || tree->hdr.bthFree > tree->hdr.bthNNodes) {
        ERROR(EIO, "invalid classic HFS tree allocation geometry");
        return -1;
    }
    while (tree->hdr.bthFree < required) {
        if (tree != &volume->ext && ClassicHfsReserveTreeSpace(&volume->ext, 1) < 0)
            return -1;
        int blocks = ClassicHfsExtendForkAllocation(&tree->f);
        if (blocks < 0)
            return -1;
        uint64_t added = (uint64_t)blocks * (blockBytes / ClassicHfsNodeSize);
        uint32_t previous = tree->hdr.bthNNodes;
        if (!added || added + previous > tree->f.cat.u.fil.filPyLen / ClassicHfsNodeSize) {
            ERROR(EIO, "classic HFS tree growth exceeds its backing fork");
            return -1;
        }
        tree->hdr.bthNNodes += (uint32_t)added;
        tree->hdr.bthFree += (uint32_t)added - (previous == 0);
        tree->flags |= HFS_UPDATE_BTHDR;
        volume->flags |= HFS_UPDATE_ALTMDB;
        if (ClassicHfsGrowTreeMap(tree) < 0)
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

int ClassicHfsDeleteTreeKey(btree *tree, unsigned char *key)
{
    node root;
    root.bt = tree;
    root.nnum = tree->hdr.bthRoot;
    unsigned depth = tree->hdr.bthDepth;
    if (ClassicHfsCheckSearchRoot(&tree->hdr) != 1 || !tree->hdr.bthNRecs) {
        ERROR(EIO, "invalid or empty classic HFS deletion root");
        return -1;
    }
    if (bt_getnode(&root) < 0)
        return -1;
    if (root.nd.ndFLink || root.nd.ndBLink ||
        (depth == 1 && (tree->hdr.bthFNode != root.nnum || tree->hdr.bthLNode != root.nnum ||
                        tree->hdr.bthNRecs != root.nd.ndNRecs))) {
        ERROR(EIO, "inconsistent classic HFS deletion root");
        return -1;
    }
    unsigned char record[HFS_MAXRECLEN];
    int changed = 0;
    if (bt_deletex(&root, depth, key, record, &changed) < 0)
        return -1;
    if (depth > 1 && root.nd.ndNRecs == 1) {
        uint32_t number;
        int slot;
        int route = ClassicHfsLookupLevel(root.data, &root.nd, root.roff, depth,
            tree == &tree->f.vol->cat, HFS_NODEREC(root, 0), tree->compare,
            &slot, tree->hdr.bthNNodes, &number, ClassicHfsDelete);
        if (route != ClassicHfsVisitChild || number == root.nnum) {
            ERROR(EIO, "invalid classic HFS root child reference");
            return -1;
        }
        node child;
        child.bt = tree;
        child.nnum = number;
        if (bt_getnode(&child) < 0)
            return -1;
        if (!ClassicHfsPromotableChild(&child.nd, depth, number, tree->hdr.bthFNode, tree->hdr.bthLNode)) {
            ERROR(EIO, "invalid classic HFS child for root promotion");
            return -1;
        }
        if (ClassicHfsFreeTreeNode(&root) < 0)
            return -1;
        tree->hdr.bthRoot = number;
        --tree->hdr.bthDepth;
    } else if (depth == 1 && !root.nd.ndNRecs) {
        if (ClassicHfsFreeTreeNode(&root) < 0)
            return -1;
        tree->hdr.bthDepth = 0;
        tree->hdr.bthRoot = tree->hdr.bthFNode = tree->hdr.bthLNode = 0;
    }
    --tree->hdr.bthNRecs;
    tree->flags |= HFS_UPDATE_BTHDR;
    return 0;
}
