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
# include "btree.h"
# include "node.h"

# define NODESPACE(n)  \
  (HFS_BLOCKSZ - (n).roff[(n).nd.ndNRecs] - 2 * ((n).nd.ndNRecs + 1))

/*
 * NAME:	node->init()
 * DESCRIPTION:	construct an empty node
 */
void n_init(node *np, btree *bt, int type, int height)
{
  np->bt   = bt;
  np->nnum = -1;

  np->nd.ndFLink   = 0;
  np->nd.ndBLink   = 0;
  np->nd.ndType    = type;
  np->nd.ndNHeight = height;
  np->nd.ndNRecs   = 0;
  np->nd.ndResv2   = 0;

  np->rnum    = -1;
  np->roff[0] = 0x00e;

  memset(np->data, 0, sizeof(np->data));
}

/*
 * NAME:	node->new()
 * DESCRIPTION:	allocate a new b*-tree node
 */
int n_new(node *np)
{
  btree *bt = np->bt;
  uint32_t num = ClassicHfsClaimTreeNode(bt->map, bt->mapsz, bt->hdr.bthNNodes,
                                         bt->hdr.bthFree);
  if (!num) {
    ERROR(EIO, "no usable free node in classic HFS B-tree bitmap");
    return -1;
  }
  np->nnum = num;
  --bt->hdr.bthFree;

  bt->flags |= HFS_UPDATE_BTHDR;

  return 0;
}

/*
 * NAME:	node->free()
 * DESCRIPTION:	deallocate a b*-tree node
 */
int n_free(node *np)
{
  btree *bt = np->bt;
  if (!ClassicHfsReleaseTreeNode(bt->map, bt->mapsz, bt->hdr.bthNNodes,
                                 bt->hdr.bthFree, np->nnum)) {
    ERROR(EIO, "invalid classic HFS B-tree node release");
    return -1;
  }
  ++bt->hdr.bthFree;
  bt->flags |= HFS_UPDATE_BTHDR;
  return 0;
}

/*
 * NAME:	node->compact()
 * DESCRIPTION:	clean up a node, removing deleted records
 */
static int compact_node(node *np)
{
  if (!ClassicHfsCompactNode(np->data, np->roff, &np->nd.ndNRecs)) {
    ERROR(EIO, "invalid classic HFS node offsets during compaction");
    return -1;
  }
  return 0;
}

/*
 * NAME:	node->search()
 * DESCRIPTION:	locate a record in a node, or the record it should follow
 */
int n_search(node *np, unsigned char *key)
{
  return ClassicHfsSearchNode(np->data, np->roff, np->nd.ndNRecs,
                              key, np->bt->compare, &np->rnum);
}

/*
 * NAME:	node->index()
 * DESCRIPTION:	create an index record from a key and node pointer
 */
int n_index(node *child, unsigned char *record, int *reclen)
{
  int catalog = child->bt == &child->bt->f.vol->cat;
  int size = child->nd.ndNRecs ? ClassicHfsWriteIndexRecord(record, catalog ? 42 : 12,
      HFS_NODEREC(*child, 0), child->roff[1] - child->roff[0], catalog, child->nnum) : 0;
  if (!size) {
    ERROR(EIO, "invalid classic HFS separator key");
    return -1;
  }
  if (reclen)
    *reclen = size;
  return 0;
}

// Load and validate the old forward link before publishing either sibling.
static int prepare_successor(node *next, node *previous, unsigned long replacement)
{
  next->bt = previous->bt;
  next->nnum = previous->nd.ndFLink;
  if (previous->nd.ndType == -1 &&
      ((next->nnum == 0) != (previous->bt->hdr.bthLNode == previous->nnum))) {
    ERROR(EIO, "inconsistent classic HFS last leaf");
    return -1;
  }
  if (!next->nnum)
    return 0;
  if (bt_getnode(next) < 0)
    return -1;
  if (!ClassicHfsRelinkSuccessor(&next->nd, next->nnum, &previous->nd, previous->nnum, replacement)) {
    ERROR(EIO, "inconsistent classic HFS successor link");
    return -1;
  }
  return 0;
}

/*
 * NAME:	node->split()
 * DESCRIPTION:	divide a node into two and insert a record
 */
int n_split(node *left, unsigned char *record, int *reclen)
{
  node right, next;
  right = *left;
  if (!ClassicHfsSplitNodeRecords(left->data, left->roff, &left->nd.ndNRecs,
                                  right.data, right.roff, &right.nd.ndNRecs,
                                  left->rnum + 1, record, *reclen)) {
    ERROR(EIO, "classic HFS records cannot fit in split nodes");
    return -1;
  }
  right.nd.ndBLink = left->nnum;
  if (n_new(&right) < 0)
    return -1;
  if (prepare_successor(&next, left, right.nnum) < 0)
    return -1;
  left->nd.ndFLink = right.nnum;

  /* store the new/modified nodes */

  if (bt_putnode(left) < 0 ||
      bt_putnode(&right) < 0)
    return -1;

  /* create an index record for the new node in the parent */

  if (n_index(&right, record, reclen) < 0)
    return -1;

  /* update link pointers */

  if (left->bt->hdr.bthLNode == left->nnum)
    {
      left->bt->hdr.bthLNode = right.nnum;
      left->bt->flags |= HFS_UPDATE_BTHDR;
    }

  if (next.nnum && bt_putnode(&next) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	node->insertx()
 * DESCRIPTION:	insert a record into a node (which must already have room)
 */
int n_insertx(node *np, unsigned char *record, int reclen)
{
  if (!ClassicHfsInsertNodeRecord(np->data, np->roff, &np->nd.ndNRecs,
                                  np->rnum + 1, record, reclen)) {
    ERROR(EIO, "record does not fit in classic HFS B-tree node");
    return -1;
  }
  return 0;
}

/*
 * NAME:	node->insert()
 * DESCRIPTION:	insert a new record into a node; return a record for parent
 */
int n_insert(node *np, unsigned char *record, int *reclen)
{
  if (compact_node(np) < 0)
    return -1;

  /* check for free space */

  if (np->nd.ndNRecs >= HFS_MAXRECS ||
      *reclen + 2 > NODESPACE(*np))
    return n_split(np, record, reclen);

  if (n_insertx(np, record, *reclen) < 0)
    return -1;
  *reclen = 0;

  return bt_putnode(np);
}

/*
 * NAME:	node->merge()
 * DESCRIPTION:	combine two nodes into a single node
 */
static int finish_merge(node *right, node *left, unsigned char *record, int *flag)
{
  /* update link pointers */

  node next;
  if (prepare_successor(&next, right, left->nnum) < 0)
    return -1;
  left->nd.ndFLink = right->nd.ndFLink;

  if (bt_putnode(left) < 0)
    return -1;

  if (right->bt->hdr.bthLNode == right->nnum)
    {
      right->bt->hdr.bthLNode = left->nnum;
      right->bt->flags |= HFS_UPDATE_BTHDR;
    }

  if (next.nnum && bt_putnode(&next) < 0)
    return -1;

  if (n_free(right) < 0)
    return -1;

  HFS_RECKEYLEN(record) = 0;
  *flag = 1;

  return 0;
}

/*
 * NAME:	node->delete()
 * DESCRIPTION:	remove a record from a node
 */
int n_delete(node *np, unsigned char *record, int *flag)
{
  node left;
  unsigned char *rec;

  rec = HFS_NODEREC(*np, np->rnum);

  HFS_RECKEYLEN(rec) = 0;
  if (compact_node(np) < 0)
    return -1;

  /* see if we can merge with our left sibling */

  left.bt   = np->bt;
  left.nnum = np->nd.ndBLink;

  if (left.nnum > 0)
    {
      if (bt_getnode(&left) < 0)
	return -1;

      if (left.nnum == np->nnum || left.nd.ndFLink != np->nnum ||
          left.nd.ndType != np->nd.ndType || left.nd.ndNHeight != np->nd.ndNHeight) {
        ERROR(EIO, "inconsistent classic HFS B-tree left sibling");
        return -1;
      }
      if (ClassicHfsMergeNodeRecords(left.data, left.roff, &left.nd.ndNRecs,
                                     np->data, np->roff, np->nd.ndNRecs))
        return finish_merge(np, &left, record, flag);
    }

  if (np->rnum == 0 && np->nd.ndNRecs)
    {
      /* special case: first record changed; update parent record key */

      if (n_index(np, record, 0) < 0)
        return -1;
      *flag = 1;
    }

  return bt_putnode(np);
}
