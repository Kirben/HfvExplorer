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
#include "ClassicHfsNodeIO.h"
# include "libhfs/block.h"
# include "libhfs/file.h"
# include "libhfs/btree.h"
# include "libhfs/node.h"
#include "ClassicHfsGrowTreeMap.h"

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

// Keep the root in the caller's storage for subsequent root split or collapse.
static int ClassicHfsReadMutationPath(node *root, unsigned depth, unsigned char *key,
                                      ClassicHfsSearchPurpose purpose, node **children)
{
    *children = NULL;
    if (!depth || depth > INT8_MAX) {
        ERROR(EIO, "invalid classic HFS mutation depth");
        return -1;
    }
    if (depth > 1) {
        *children = (node *)calloc(depth - 1, sizeof(node));
        if (!*children) {
            ERROR(ENOMEM, "cannot allocate classic HFS mutation path");
            return -1;
        }
    }
    node *current = root;
    btree *tree = root->bt;
    for (unsigned level = 0; level < depth; ++level) {
        uint32_t child;
        int route = ClassicHfsLookupLevel(current->data, &current->nd, current->roff, depth - level,
                                          tree == &tree->f.vol->cat, key, tree->compare, &current->rnum,
                                          tree->hdr.bthNNodes, &child, purpose);
        if (level + 1 == depth) {
            if (route == (purpose == ClassicHfsInsert ? ClassicHfsMissingKey : ClassicHfsFoundKey))
                return 0;
            if (route == ClassicHfsFoundKey) {
                ERROR(EIO, "classic HFS record already exists");
                break;
            }
        }
        if (route != ClassicHfsVisitChild || level + 1 == depth) {
            ERROR(EIO, "invalid classic HFS mutation path or missing key");
            break;
        }
        current = &(*children)[level];
        current->bt = tree;
        current->nnum = child;
        if (ClassicHfsReadTreeNode(current) < 0)
            break;
    }
    free(*children);
    *children = NULL;
    return -1;
}

static int ClassicHfsInsertAlongPath(node *root, unsigned depth, unsigned char *record, int *length)
{
    node *children;
    if (ClassicHfsReadMutationPath(root, depth, record, ClassicHfsInsert, &children) < 0)
        return -1;
    unsigned level = depth - 1;
    node *current = level ? &children[level - 1] : root;
    int result = ClassicHfsInsertAtCursor(current, record, length);
    while (level && result == 0) {
        --level;
        node *parent = level ? &children[level - 1] : root;
        if (parent->rnum < 0) {
            result = ClassicHfsBuildSeparator(current, HFS_NODEREC(*parent, 0), parent->roff[1] - parent->roff[0], NULL);
            if (result < 0)
                break;
            parent->rnum = 0;
            if (!*length)
                result = ClassicHfsWriteTreeNode(parent);
        }
        if (*length)
            result = ClassicHfsInsertAtCursor(parent, record, length);
        current = parent;
    }
    free(children);
    return result;
}

static int read_mutation_root(node *root)
{
    if (ClassicHfsReadTreeNode(root) < 0)
        return -1;
    if (!ClassicHfsValidMutationRoot(&root->bt->hdr, root->nd.ndFLink, root->nd.ndBLink,
                                     root->nd.ndNRecs)) {
        ERROR(EIO, "inconsistent classic HFS mutation root");
        return -1;
    }
    return 0;
}

int ClassicHfsInsertTreeRecord(btree *tree, const unsigned char *input, int length)
{
    unsigned char record[HFS_MAXRECLEN];
    int catalog = tree == &tree->f.vol->cat;
    if (!input || length <= 0 || (length & 1) || (size_t)length > sizeof(record) ||
        !(catalog ? ClassicHfsCatalogPayload(input, length, NULL) : ClassicHfsValidExtentRecord(input, length))) {
        ERROR(EIO, "invalid classic HFS record for insertion");
        return -1;
    }
    int state = ClassicHfsCheckSearchRoot(&tree->hdr);
    if (state < 0 || tree->hdr.bthNRecs == UINT32_MAX) {
        ERROR(EIO, "invalid classic HFS insertion root");
        return -1;
    }
    node root;
    if (!state) {
        ClassicHfsInitTreeNode(&root, tree, ndLeafNode, 1);
        if (!ClassicHfsInsertNodeRecord(root.data, root.roff, &root.nd.ndNRecs, 0, input, length)) {
            ERROR(EIO, "classic HFS record exceeds leaf capacity");
            return -1;
        }
        if (ClassicHfsAllocateTreeNode(&root) < 0 || ClassicHfsWriteTreeNode(&root) < 0)
            return -1;
        tree->hdr.bthDepth = 1;
        tree->hdr.bthRoot = tree->hdr.bthFNode = tree->hdr.bthLNode = root.nnum;
    } else {
        root.bt = tree;
        root.nnum = tree->hdr.bthRoot;
        memcpy(record, input, length);
        if (read_mutation_root(&root) < 0 ||
            ClassicHfsInsertAlongPath(&root, tree->hdr.bthDepth, record, &length) < 0)
            return -1;
        if (length) {
            unsigned char left[HFS_MAXRECLEN];
            int leftLength;
            if (ClassicHfsBuildSeparator(&root, left, sizeof(left), &leftLength) < 0)
                return -1;
            if (tree->hdr.bthDepth >= INT8_MAX || tree->compare(left, record) >= 0) {
                ERROR(EIO, "invalid classic HFS root split");
                return -1;
            }
            node parent;
            ClassicHfsInitTreeNode(&parent, tree, ndIndxNode, tree->hdr.bthDepth + 1);
            if (!ClassicHfsInsertNodeRecord(parent.data, parent.roff, &parent.nd.ndNRecs, 0, left, leftLength) ||
                !ClassicHfsInsertNodeRecord(parent.data, parent.roff, &parent.nd.ndNRecs, 1, record, length)) {
                ERROR(EIO, "classic HFS root separators exceed node capacity");
                return -1;
            }
            if (ClassicHfsAllocateTreeNode(&parent) < 0 || ClassicHfsWriteTreeNode(&parent) < 0)
                return -1;
            tree->hdr.bthRoot = parent.nnum;
            ++tree->hdr.bthDepth;
        }
    }
    ++tree->hdr.bthNRecs;
    tree->flags |= HFS_UPDATE_BTHDR;
    return 0;
}

// Read the path before changing the leaf. Keep each parent and its selected
// slot until the child's separator has been updated or removed.
static int ClassicHfsDeleteAlongPath(node *root, unsigned depth, unsigned char *key)
{
    node *children;
    if (ClassicHfsReadMutationPath(root, depth, key, ClassicHfsDelete, &children) < 0)
        return -1;
    node *current = depth > 1 ? &children[depth - 2] : root;
    int result = -1;
    unsigned char rootSeparator[HFS_MAXRECLEN];
    int removeRecord = 1;
    for (unsigned height = 1; height <= depth; ++height) {
        unsigned level = depth - height;
        node *parent = level > 1 ? &children[level - 2] : root;
        unsigned char *separator = level ? HFS_NODEREC(*parent, parent->rnum) : rootSeparator;
        unsigned capacity = level ? parent->roff[parent->rnum + 1] - parent->roff[parent->rnum] : sizeof(rootSeparator);
        int changed = 0;
        if (removeRecord) {
            if (ClassicHfsDeleteNodeRecord(current, separator, capacity, &changed) < 0)
                goto done;
        } else {
            if (current->rnum == 0) {
                if (ClassicHfsBuildSeparator(current, separator, capacity, NULL) < 0)
                    goto done;
                changed = 1;
            }
            if (ClassicHfsWriteTreeNode(current) < 0)
                goto done;
        }
        if (!level || !changed)
            break;
        removeRecord = separator[0] == 0;
        current = parent;
    }
    result = 0;
done:
    free(children);
    return result;
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
    if (read_mutation_root(&root) < 0)
        return -1;
    if (ClassicHfsDeleteAlongPath(&root, depth, key) < 0)
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
        if (ClassicHfsReadTreeNode(&child) < 0)
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
