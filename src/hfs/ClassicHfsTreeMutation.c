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
#include "ClassicHfsCatalogLeaf.h"
#include "ClassicHfsPath.h"
#include "ClassicHfsTime.h"
#include "libhfs/volume.h"
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

int ClassicHfsFindCatalogEntry(hfsvol *volume, long parent, char *name, CatDataRec *data,
                              char *storedName, node *cursor)
{
    unsigned char search[HFS_CATKEYLEN];
    if (ClassicHfsWriteCatalogKey(search, parent, name) < 0) {
        ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
        return -1;
    }
    node local;
    if (!cursor)
        cursor = &local;
    int found = ClassicHfsFindTreeKey(&volume->cat, search, cursor);
    if (found <= 0)
        return found;
    CatKeyRec key;
    if (!ClassicHfsReadLeafEntry(cursor->data, cursor->roff, cursor->nd.ndNRecs, cursor->rnum, &key, data)) {
        ERROR(EIO, "invalid classic HFS catalog record");
        return -1;
    }
    if (storedName)
        strcpy(storedName, key.ckrCName);
    return 1;
}

int ClassicHfsFindCatalogThread(hfsvol *volume, long id, CatDataRec *thread, node *cursor, int type)
{
    CatDataRec record;
    int found = ClassicHfsFindCatalogEntry(volume, id, "", &record, NULL, cursor);
    if (found <= 0)
        return found;
    if (record.cdrType != type) {
        ERROR(EIO, "bad classic HFS thread record");
        return -1;
    }
    if (thread)
        *thread = record;
    return 1;
}

int ClassicHfsStoreCatalogEntry(CatDataRec *record, node *cursor)
{
    if (!cursor->bt || !cursor->bt->f.vol || cursor->bt != &cursor->bt->f.vol->cat ||
        (unsigned char)cursor->nd.ndType != ndLeafNode || cursor->nd.ndNHeight != 1) {
        ERROR(EIO, "catalog update requires a classic HFS catalog leaf");
        return -1;
    }
    node updated = *cursor;
    if (!ClassicHfsUpdateLeafEntry(updated.data, updated.roff, updated.nd.ndNRecs, updated.rnum, record)) {
        ERROR(EIO, "invalid classic HFS catalog update");
        return -1;
    }
    if (ClassicHfsWriteTreeNode(&updated) < 0)
        return -1;
    *cursor = updated;
    return 0;
}

static HFV_THREAD_LOCAL int ignoreCdLabel;

void set_invalid_cd_hack(int enabled)
{
    ignoreCdLabel = enabled;
}

int ClassicHfsResolvePath(hfsvol *volume, char *path, CatDataRec *record,
                          long *parent, char *storedName, node *position)
{
    const char *cursor = path;
    long directory = volume->cwd;
    if (parent && *path)
        *parent = 0;
    switch (ClassicHfsBeginPath(&cursor, volume->mdb.drVN, ignoreCdLabel)) {
    case ClassicHfsPathEmpty: ERROR(ENOENT, "empty path"); return -1;
    case ClassicHfsPathWrongVolume: ERROR(ENOENT, "path does not name the selected HFS volume"); return -1;
    case ClassicHfsPathTooLong: ERROR(ENAMETOOLONG, NULL); return -1;
    case ClassicHfsPathAbsolute: directory = HFS_CNID_ROOTPAR; break;
    }

    for (;;) {
        char name[HFS_MAX_FLEN + 1];
        int part = ClassicHfsNextPathPart(&cursor, name);
        if (part == ClassicHfsPathTooLong) {
            ERROR(ENAMETOOLONG, NULL);
            return -1;
        }
        if (part == ClassicHfsPathEnd || part == ClassicHfsPathParent) {
            CatDataRec thread;
            int found = ClassicHfsFindCatalogThread(volume, directory, &thread, NULL, cdrThdRec);
            if (found <= 0)
                return found;
            long owner = thread.u.dthd.thdParID;
            found = ClassicHfsFindCatalogEntry(volume, owner, thread.u.dthd.thdCName, record,
                                               part == ClassicHfsPathEnd ? storedName : NULL,
                                               part == ClassicHfsPathEnd ? position : NULL);
            if (found < 0)
                return -1;
            if (!found || record->cdrType != cdrDirRec || record->u.dir.dirDirID != directory) {
                ERROR(EIO, "classic HFS directory thread identifies another or missing entry");
                return -1;
            }
            if (part == ClassicHfsPathEnd) {
                if (parent)
                    *parent = owner;
                return 1;
            }
            directory = owner;
            continue;
        }

        if (parent)
            *parent = directory;
        int found = ClassicHfsFindCatalogEntry(volume, directory, name, record, storedName, position);
        if (found <= 0) {
            if (!found && *cursor && parent)
                *parent = 0;
            if (!found && !*cursor && storedName)
                strcpy(storedName, name);
            return found;
        }
        if (record->cdrType != cdrDirRec && record->cdrType != cdrFilRec) {
            ERROR(EIO, "unexpected classic HFS catalog record");
            return -1;
        }
        if (!*cursor)
            return 1;
        if (record->cdrType != cdrDirRec) {
            ERROR(ENOTDIR, "invalid pathname");
            return -1;
        }
        directory = record->u.dir.dirDirID;
    }
}

int ClassicHfsAdjustEntryCount(hfsvol *volume, long parent, int directory, int delta)
{
    if (delta != 1 && delta != -1) {
        ERROR(EINVAL, "invalid classic HFS entry count adjustment");
        return -1;
    }
    if (parent == HFS_CNID_ROOTPAR && directory)
        return 0;

    unsigned long *total = directory ? &volume->mdb.drDirCnt : &volume->mdb.drFilCnt;
    uint16_t *rootCount = directory ? &volume->mdb.drNmRtDirs : &volume->mdb.drNmFls;
    if ((delta > 0 && (*total == UINT32_MAX || (parent == HFS_CNID_ROOTDIR && *rootCount == UINT16_MAX))) ||
        (delta < 0 && (!*total || (parent == HFS_CNID_ROOTDIR && !*rootCount)))) {
        ERROR(EIO, "classic HFS volume entry count out of range");
        return -1;
    }

    CatDataRec thread, entry;
    node cursor;
    if (ClassicHfsFindCatalogThread(volume, parent, &thread, NULL, cdrThdRec) <= 0 ||
        ClassicHfsFindCatalogEntry(volume, thread.u.dthd.thdParID, thread.u.dthd.thdCName,
                                   &entry, NULL, &cursor) <= 0 ||
        entry.cdrType != cdrDirRec || entry.u.dir.dirDirID != parent) {
        ERROR(EIO, "cannot identify classic HFS parent directory");
        return -1;
    }
    if ((delta > 0 && entry.u.dir.dirVal == UINT16_MAX) || (delta < 0 && !entry.u.dir.dirVal)) {
        ERROR(EIO, "classic HFS directory valence out of range");
        return -1;
    }
    entry.u.dir.dirVal += delta;
    entry.u.dir.dirMdDat = ClassicHfsFromUnixSeconds(time(NULL));
    if (ClassicHfsStoreCatalogEntry(&entry, &cursor) < 0)
        return -1;
    *total += delta;
    if (parent == HFS_CNID_ROOTDIR)
        *rootCount += delta;
    volume->flags |= HFS_UPDATE_MDB;
    return 0;
}

int ClassicHfsCheckNewCatalogId(hfsvol *volume, int root)
{
    uint32_t id = volume->mdb.drNxtCNID;
    if (root ? id != HFS_CNID_ROOTDIR : id < 16) {
        ERROR(EIO, "invalid classic HFS next catalog ID");
        return -1;
    }
    if (id == UINT32_MAX) {
        ERROR(ENOSPC, "classic HFS catalog IDs exhausted");
        return -1;
    }
    return 0;
}

static int encode_catalog_entry(unsigned char bytes[HFS_CATRECMAXLEN], long parent,
                                const char *name, const CatDataRec *entry)
{
    int keySize = ClassicHfsWriteCatalogKey(bytes, parent, name);
    if (keySize < 0) {
        ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
        return -1;
    }
    unsigned payload = ClassicHfsEncodeCatalogRecord(bytes + keySize, HFS_CATRECMAXLEN - keySize, entry);
    if (!payload) {
        ERROR(EIO, "invalid classic HFS catalog record size");
        return -1;
    }
    return keySize + payload;
}

int ClassicHfsCreateDirectoryRecords(hfsvol *volume, long parent, char *name)
{
    if (ClassicHfsCheckNewCatalogId(volume, parent == HFS_CNID_ROOTPAR) < 0)
        return -1;
    uint32_t id = volume->mdb.drNxtCNID;
    CatDataRec directory = {0}, thread = {0};
    directory.cdrType = cdrDirRec;
    directory.u.dir.dirDirID = id;
    directory.u.dir.dirCrDat = directory.u.dir.dirMdDat = ClassicHfsFromUnixSeconds(time(NULL));
    unsigned char entryBytes[HFS_CATRECMAXLEN], threadBytes[HFS_CATRECMAXLEN];
    int entrySize = encode_catalog_entry(entryBytes, parent, name, &directory);
    if (entrySize < 0)
        return -1;
    thread.cdrType = cdrThdRec;
    thread.u.dthd.thdParID = parent;
    strcpy(thread.u.dthd.thdCName, name); // Length checked by the catalog key encoder.
    int threadSize = encode_catalog_entry(threadBytes, id, "", &thread);
    if (threadSize < 0 || ClassicHfsReserveTreeSpace(&volume->cat, 2) < 0 ||
        ClassicHfsInsertTreeRecord(&volume->cat, entryBytes, entrySize) < 0 ||
        ClassicHfsInsertTreeRecord(&volume->cat, threadBytes, threadSize) < 0 ||
        ClassicHfsAdjustEntryCount(volume, parent, 1, 1) < 0)
        return -1;
    volume->mdb.drNxtCNID = id + 1;
    volume->flags |= HFS_UPDATE_MDB;
    return 0;
}

static int create_file_record(hfsvol *volume, long parent, char *name,
                              const char *type, const char *creator)
{
    CatDataRec file = {0};
    file.cdrType = cdrFilRec;
    file.u.fil.filFlNum = volume->mdb.drNxtCNID;
    file.u.fil.filCrDat = file.u.fil.filMdDat = ClassicHfsFromUnixSeconds(time(NULL));
    file.u.fil.filUsrWds.fdType = ClassicHfsRead32((const unsigned char *)type);
    file.u.fil.filUsrWds.fdCreator = ClassicHfsRead32((const unsigned char *)creator);

    unsigned char bytes[HFS_CATRECMAXLEN];
    int size = encode_catalog_entry(bytes, parent, name, &file);
    if (size < 0 || ClassicHfsReserveTreeSpace(&volume->cat, 1) < 0 ||
        ClassicHfsInsertTreeRecord(&volume->cat, bytes, size) < 0 ||
        ClassicHfsAdjustEntryCount(volume, parent, 0, 1) < 0)
        return -1;
    ++volume->mdb.drNxtCNID;
    volume->flags |= HFS_UPDATE_MDB;
    return 0;
}

static int create_catalog_path(hfsvol *volume, char *path, const char *type, const char *creator)
{
    if (ClassicHfsCurrentVolume(&volume) < 0)
        return -1;
    char name[HFS_MAX_FLEN + 1];
    long parent = 0;
    CatDataRec existing;
    int found = ClassicHfsResolvePath(volume, path, &existing, &parent, name, NULL);
    if (found < 0)
        return -1;
    if (found) {
        ERROR(EEXIST, "classic HFS entry already exists");
        return -1;
    }
    if (!parent || parent == HFS_CNID_ROOTPAR) {
        ERROR(parent ? EINVAL : ENOENT, "invalid classic HFS creation parent");
        return -1;
    }
    if (volume->flags & HFS_READONLY) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if (ClassicHfsCheckNewCatalogId(volume, 0) < 0 || ClassicHfsBeginTransaction(volume) < 0)
        return -1;
    int result = type ? create_file_record(volume, parent, name, type, creator)
                      : ClassicHfsCreateDirectoryRecords(volume, parent, name);
    return ClassicHfsEndTransaction(volume, result);
}

int hfs_create(hfsvol *volume, char *path, char *type, char *creator)
{
    if (!type || !creator) {
        ERROR(EINVAL, "classic HFS file creation requires type and creator codes");
        return -1;
    }
    return create_catalog_path(volume, path, type, creator);
}

int hfs_mkdir(hfsvol *volume, char *path)
{
    return create_catalog_path(volume, path, NULL, NULL);
}

// Children sort immediately after their directory thread. Check the first
// successor even when the directory record claims a zero valence.
static int verify_empty_directory(hfsvol *volume, node *leaf, long id)
{
    int slot = leaf->rnum + 1;
    if (slot == leaf->nd.ndNRecs) {
        unsigned long previous = leaf->nnum;
        if (!leaf->nd.ndFLink && previous == volume->cat.hdr.bthLNode)
            return 0;
        if (!leaf->nd.ndFLink || leaf->nd.ndFLink == previous || previous == volume->cat.hdr.bthLNode)
            goto invalid;
        leaf->nnum = leaf->nd.ndFLink;
        if (ClassicHfsReadTreeNode(leaf) < 0)
            return -1;
        if (leaf->nd.ndType != -1 || leaf->nd.ndNHeight != 1 || leaf->nd.ndBLink != previous)
            goto invalid;
        slot = 0;
    }
    CatKeyRec key;
    if (!ClassicHfsReadLeafEntry(leaf->data, leaf->roff, leaf->nd.ndNRecs, slot, &key, NULL))
        goto invalid;
    if (key.ckrParID == (uint32_t)id) {
        ERROR(ENOTEMPTY, "classic HFS catalog still contains directory children");
        return -1;
    }
    if (key.ckrParID > (uint32_t)id)
        return 0;
invalid:
    ERROR(EIO, "invalid classic HFS catalog order or leaf link");
    return -1;
}

static int remove_catalog_records(hfsvol *volume, hfsfile *item, int directory, int hasThread)
{
    long id = directory ? item->cat.u.dir.dirDirID : item->cat.u.fil.filFlNum;
    if (!directory) {
        item->cat.u.fil.filLgLen = item->cat.u.fil.filRLgLen = 0;
        for (int fork = 0; fork < 2; ++fork) {
            ClassicHfsSelectFork(item, fork);
            if (ClassicHfsTrimForkAllocation(item) < 0)
                return -1;
        }
    }
    unsigned char key[HFS_CATKEYLEN];
    if (ClassicHfsWriteCatalogKey(key, item->parid, item->name) < 0 ||
        ClassicHfsDeleteTreeKey(&volume->cat, key) < 0)
        return -1;
    if (hasThread) {
        ClassicHfsWriteCatalogKey(key, id, "");
        if (ClassicHfsDeleteTreeKey(&volume->cat, key) < 0)
            return -1;
    }
    return ClassicHfsAdjustEntryCount(volume, item->parid, directory, -1);
}

static int remove_catalog_path(hfsvol *volume, char *path, int directory)
{
    hfsfile item = {0};
    if (ClassicHfsCurrentVolume(&volume) < 0)
        return -1;
    item.vol = volume;
    int found = ClassicHfsResolvePath(volume, path, &item.cat, &item.parid, item.name, NULL);
    if (!found) {
        ERROR(ENOENT, "classic HFS entry not found");
        return -1;
    }
    if (found < 0)
        return -1;
    if (item.cat.cdrType != (directory ? cdrDirRec : cdrFilRec)) {
        ERROR(directory ? ENOTDIR : EISDIR, "unexpected classic HFS entry type");
        return -1;
    }
    if (item.parid == HFS_CNID_ROOTPAR) {
        ERROR(EINVAL, "cannot remove classic HFS root");
        return -1;
    }
    long id = directory ? item.cat.u.dir.dirDirID : item.cat.u.fil.filFlNum;
    if (directory && item.cat.u.dir.dirVal) {
        ERROR(ENOTEMPTY, "classic HFS directory is not empty");
        return -1;
    }
    if (!directory) {
        for (hfsfile *openFile = volume->files; openFile; openFile = openFile->next)
            if (openFile->cat.u.fil.filFlNum == id) {
                ERROR(EBUSY, "file is open; close it before deleting");
                return -1;
            }
    }
    CatDataRec thread;
    node leaf;
    int hasThread = ClassicHfsFindCatalogThread(volume, id, &thread, &leaf, directory ? cdrThdRec : cdrFThdRec);
    if (hasThread < 0)
        return -1;
    if ((!hasThread && directory) || (hasThread && (thread.u.dthd.thdParID != item.parid ||
        ClassicHfsCompareNames(thread.u.dthd.thdCName, item.name)))) {
        ERROR(EIO, "classic HFS thread does not identify the entry being removed");
        return -1;
    }
    if ((directory && verify_empty_directory(volume, &leaf, id) < 0) || ClassicHfsBeginTransaction(volume) < 0)
        return -1;
    int result = remove_catalog_records(volume, &item, directory, hasThread);
    return ClassicHfsEndTransaction(volume, result);
}

int hfs_delete(hfsvol *volume, char *path)
{
    return remove_catalog_path(volume, path, 0);
}

int hfs_rmdir(hfsvol *volume, char *path)
{
    return remove_catalog_path(volume, path, 1);
}
