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
  unsigned long num;

  if (bt->hdr.bthFree == 0)
    {
      ERROR(EIO, "b*-tree full");
      return -1;
    }

  num = 0;
  while (num < bt->hdr.bthNNodes && ClassicHfsBitIsSet(bt->map, num))
    ++num;

  if (num == bt->hdr.bthNNodes)
    {
      ERROR(EIO, "free b*-tree node not found");
      return -1;
    }

  np->nnum = num;

  ClassicHfsSetBit(bt->map, num);
  --bt->hdr.bthFree;

  bt->flags |= HFS_UPDATE_BTHDR;

  return 0;
}

/*
 * NAME:	node->free()
 * DESCRIPTION:	deallocate a b*-tree node
 */
void n_free(node *np)
{
  btree *bt = np->bt;

  ClassicHfsClearBit(bt->map, np->nnum);
  ++bt->hdr.bthFree;

  bt->flags |= HFS_UPDATE_BTHDR;
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
  btree *bt = np->bt;
  int i, comp = -1;

  for (i = np->nd.ndNRecs; i--; )
    {
      unsigned char *rec;

      rec = HFS_NODEREC(*np, i);

      if (HFS_RECKEYLEN(rec) == 0)
	continue;  /* deleted record */

      comp = bt->compare(rec, key);

      if (comp <= 0)
	break;
    }

  np->rnum = i;

  return comp == 0;
}

/*
 * NAME:	node->index()
 * DESCRIPTION:	create an index record from a key and node pointer
 */
void n_index(btree *bt, unsigned char *key, unsigned long nnum,
	     unsigned char *record, int *reclen)
{
  if (bt == &bt->f.vol->cat)
    {
      /* force the key length to be 0x25 */

      HFS_RECKEYLEN(record) = 0x25;
      memset(record + 1, 0, 0x25);
      memcpy(record + 1, key + 1, HFS_RECKEYLEN(key));
    }
  else
    memcpy(record, key, HFS_RECKEYSKIP(key));

  ClassicHfsWrite32(HFS_RECDATA(record), nnum);

  if (reclen)
    *reclen = HFS_RECKEYSKIP(record) + 4;
}

/*
 * NAME:	node->split()
 * DESCRIPTION:	divide a node into two and insert a record
 */
int n_split(node *left, unsigned char *record, int *reclen)
{
  node right;
  int nrecs, i;
  unsigned char *rec;

  right = *left;
  right.nd.ndBLink = left->nnum;

  if (n_new(&right) < 0)
    return -1;

  left->nd.ndFLink = right.nnum;
  nrecs = left->nd.ndNRecs;

  for (i = 0; i < nrecs; ++i)
    {
      if (i < nrecs / 2)
	rec = HFS_NODEREC(right, i);
      else
	rec = HFS_NODEREC(*left, i);

      HFS_RECKEYLEN(rec) = 0;
    }

  if (compact_node(left) < 0 || compact_node(&right) < 0)
    return -1;

  n_search(&right, record);
  if (right.rnum >= 0) {
    if (n_insertx(&right, record, *reclen) < 0)
      return -1;
  }
  else
    {
      n_search(left, record);
      if (n_insertx(left, record, *reclen) < 0)
        return -1;
    }

  /* store the new/modified nodes */

  if (bt_putnode(left) < 0 ||
      bt_putnode(&right) < 0)
    return -1;

  /* create an index record for the new node in the parent */

  n_index(right.bt, HFS_NODEREC(right, 0), right.nnum, record, reclen);

  /* update link pointers */

  if (left->bt->hdr.bthLNode == left->nnum)
    {
      left->bt->hdr.bthLNode = right.nnum;
      left->bt->flags |= HFS_UPDATE_BTHDR;
    }

  if (right.nd.ndFLink)
    {
      node n;

      n.bt   = right.bt;
      n.nnum = right.nd.ndFLink;

      if (bt_getnode(&n) < 0)
	return -1;

      n.nd.ndBLink = right.nnum;

      if (bt_putnode(&n) < 0)
	return -1;
    }

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

  left->nd.ndFLink = right->nd.ndFLink;

  if (bt_putnode(left) < 0)
    return -1;

  if (right->bt->hdr.bthLNode == right->nnum)
    {
      right->bt->hdr.bthLNode = left->nnum;
      right->bt->flags |= HFS_UPDATE_BTHDR;
    }

  if (right->nd.ndFLink)
    {
      node n;

      n.bt   = right->bt;
      n.nnum = right->nd.ndFLink;

      if (bt_getnode(&n) < 0)
	return -1;

      n.nd.ndBLink = left->nnum;

      if (bt_putnode(&n) < 0)
	return -1;
    }

  n_free(right);

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

  if (np->rnum == 0)
    {
      /* special case: first record changed; update parent record key */

      n_index(np->bt, HFS_NODEREC(*np, 0), np->nnum, record, 0);
      *flag = 1;
    }

  return bt_putnode(np);
}
