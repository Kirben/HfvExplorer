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

# include "libhfs/internal.h"
#include "ClassicHfsBytes.h"
# include "ClassicHfsTreeLookup.h"
# include "ClassicHfsTreeNodes.h"

int ClassicHfsBuildSeparator(node *child, unsigned char *record, unsigned capacity, int *length)
{
    if (!child->nd.ndNRecs || !ClassicHfsValidOffsets(child->nd.ndNRecs, child->roff)) {
        ERROR(EIO, "invalid classic HFS separator source offsets");
        return -1;
    }
    int catalog = child->bt == &child->bt->f.vol->cat;
    int size = ClassicHfsWriteIndexRecord(record, capacity, child->data + child->roff[0],
        child->roff[1] - child->roff[0], catalog, child->nnum);
    if (!size) {
        ERROR(EIO, "invalid classic HFS separator key or destination size");
        return -1;
    }
    if (length)
        *length = size;
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
  if (ClassicHfsReadTreeNode(next) < 0)
    return -1;
  if (!ClassicHfsRelinkSuccessor(&next->nd, next->nnum, &previous->nd, previous->nnum, replacement)) {
    ERROR(EIO, "inconsistent classic HFS successor link");
    return -1;
  }
  return 0;
}

// The mutation descent already validated live keys and selected a predecessor.
int ClassicHfsInsertAtCursor(node *cursor, unsigned char *record, int *length)
{
    node left = *cursor;
    unsigned slot = (unsigned)(left.rnum + 1);
    if (ClassicHfsInsertNodeRecord(left.data, left.roff, &left.nd.ndNRecs, slot, record, *length)) {
        if (ClassicHfsWriteTreeNode(&left) < 0)
            return -1;
        *cursor = left;
        *length = 0;
        return 0;
    }
    node right = left;
    if (!ClassicHfsSplitNodeRecords(left.data, left.roff, &left.nd.ndNRecs,
                                    right.data, right.roff, &right.nd.ndNRecs, slot, record, *length)) {
        ERROR(EIO, "invalid classic HFS insertion slot or split layout");
        return -1;
    }
    right.nnum = UINT32_MAX;
    if (ClassicHfsAllocateTreeNode(&right) < 0)
        return -1;
    node next;
    unsigned char separator[HFS_MAXRECLEN];
    int separatorLength;
    right.nd.ndBLink = left.nnum;
    if (prepare_successor(&next, &left, right.nnum) < 0 ||
        ClassicHfsBuildSeparator(&right, separator, sizeof(separator), &separatorLength) < 0) {
        ClassicHfsFreeTreeNode(&right);
        return -1;
    }
    left.nd.ndFLink = right.nnum;
    if (ClassicHfsWriteTreeNode(&right) < 0 || ClassicHfsWriteTreeNode(&left) < 0 || (next.nnum && ClassicHfsWriteTreeNode(&next) < 0))
        return -1;
    if (left.bt->hdr.bthLNode == left.nnum) {
        left.bt->hdr.bthLNode = right.nnum;
        left.bt->flags |= HFS_UPDATE_BTHDR;
    }
    *cursor = left;
    memcpy(record, separator, separatorLength);
    *length = separatorLength;
    return 0;
}

// Both adjacent records have been combined in the local left node. Check the
// unlink and the bitmap release before publishing the first sibling write.
static int ClassicHfsFinishNodeMerge(node *right, node *left, unsigned char *record, int *flag)
{
    btree *tree = right->bt;
    if (right->nnum == tree->hdr.bthRoot || right->nnum == tree->hdr.bthFNode ||
        !ClassicHfsCanReleaseTreeNode(tree->map, tree->mapsz, tree->hdr.bthNNodes,
                                      tree->hdr.bthFree, right->nnum)) {
        ERROR(EIO, "inconsistent classic HFS merged node allocation or tree endpoint");
        return -1;
    }
    node successor;
    if (prepare_successor(&successor, right, left->nnum) < 0)
        return -1;
    left->nd.ndFLink = right->nd.ndFLink;
    if (ClassicHfsWriteTreeNode(left) < 0 || (successor.nnum && ClassicHfsWriteTreeNode(&successor) < 0))
        return -1;
    if (ClassicHfsFreeTreeNode(right) < 0)
        return -1;
    if (tree->hdr.bthLNode == right->nnum)
        tree->hdr.bthLNode = left->nnum;
    record[0] = 0;
    *flag = 1;
    return 0;
}

int ClassicHfsDeleteNodeRecord(node *current, unsigned char *separator, unsigned capacity, int *changed)
{
    node remaining = *current;
    if (!ClassicHfsRemoveNodeRecord(remaining.data, remaining.roff, &remaining.nd.ndNRecs,
                                    remaining.rnum)) {
        ERROR(EIO, "invalid classic HFS deletion slot or node offsets");
        return -1;
    }
    if (remaining.nd.ndBLink) {
        node left;
        left.bt = remaining.bt;
        left.nnum = remaining.nd.ndBLink;
        if (ClassicHfsReadTreeNode(&left) < 0)
            return -1;
        if (left.nnum == remaining.nnum || left.nd.ndFLink != remaining.nnum ||
            left.nd.ndType != remaining.nd.ndType || left.nd.ndNHeight != remaining.nd.ndNHeight) {
            ERROR(EIO, "inconsistent classic HFS B-tree left sibling");
            return -1;
        }
        if (ClassicHfsMergeNodeRecords(left.data, left.roff, &left.nd.ndNRecs,
                                       remaining.data, remaining.roff, remaining.nd.ndNRecs))
            return ClassicHfsFinishNodeMerge(&remaining, &left, separator, changed);
    }
    unsigned char replacement[HFS_MAXRECLEN];
    int length = 0;
    if (remaining.rnum == 0 && remaining.nd.ndNRecs && ClassicHfsBuildSeparator(&remaining, replacement, capacity < sizeof(replacement) ? capacity : sizeof(replacement), &length) < 0)
        return -1;
    if (ClassicHfsWriteTreeNode(&remaining) < 0)
        return -1;
    if (length) {
        memcpy(separator, replacement, length);
        *changed = 1;
    }
    *current = remaining;
    return 0;
}
