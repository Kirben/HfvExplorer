// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "libhfs/internal.h"
#include "ClassicHfsTreeLookup.h"
#include "libhfs/volume.h"
#include "ClassicHfsCatalogLeaf.h"
#include "ClassicHfsEntry.h"

int hfs_chdir(hfsvol *volume, char *path)
{
    CatDataRec entry;
    if (ClassicHfsCurrentVolume(&volume) < 0 ||
        ClassicHfsResolvePath(volume, path, &entry, NULL, NULL, NULL) <= 0)
        return -1;
    if (entry.cdrType != cdrDirRec) {
        ERROR(ENOTDIR, NULL);
        return -1;
    }
    volume->cwd = entry.u.dir.dirDirID;
    return 0;
}

long hfs_getcwd(hfsvol *volume)
{
    return ClassicHfsCurrentVolume(&volume) < 0 ? 0 : volume->cwd;
}

static int directory_thread(hfsvol *volume, long id, CatDataRec *thread, node *slot)
{
    if (ClassicHfsFindCatalogThread(volume, id, thread, slot, cdrThdRec) <= 0)
        return -1;
    CatDataRec entry;
    int found = ClassicHfsFindCatalogEntry(volume, thread->u.dthd.thdParID,
                                           thread->u.dthd.thdCName, &entry, NULL, NULL);
    if (found < 0)
        return -1;
    if (!found || entry.cdrType != cdrDirRec || entry.u.dir.dirDirID != id) {
        ERROR(EIO, "invalid classic HFS directory thread target");
        return -1;
    }
    return 0;
}

int hfs_setcwd(hfsvol *volume, long id)
{
    CatDataRec thread;
    if (ClassicHfsCurrentVolume(&volume) < 0 || directory_thread(volume, id, &thread, NULL) < 0)
        return -1;
    volume->cwd = id;
    return 0;
}

int hfs_dirinfo(hfsvol *volume, long *id, char *name)
{
    CatDataRec thread;
    if (ClassicHfsCurrentVolume(&volume) < 0 || directory_thread(volume, *id, &thread, NULL) < 0)
        return -1;
    if (name)
        strcpy(name, thread.u.dthd.thdCName);
    *id = thread.u.dthd.thdParID;
    return 0;
}

hfsdir *hfs_opendir(hfsvol *volume, char *path)
{
    if (ClassicHfsCurrentVolume(&volume) < 0)
        return NULL;
    hfsdir *dir = (hfsdir *)calloc(1, sizeof(*dir));
    if (!dir) {
        ERROR(ENOMEM, NULL);
        return NULL;
    }
    if (*path) {
        CatDataRec entry, thread;
        if (ClassicHfsResolvePath(volume, path, &entry, NULL, NULL, NULL) <= 0)
            goto fail;
        if (entry.cdrType != cdrDirRec) {
            ERROR(ENOTDIR, NULL);
            goto fail;
        }
        dir->dirid = entry.u.dir.dirDirID;
        if (directory_thread(volume, dir->dirid, &thread, &dir->n) < 0)
            goto fail;
        dir->leavesVisited = 1;
    } else {
        dir->vptr = hfs_mounts;
    }
    dir->vol = volume;
    dir->next = volume->dirs;
    if (dir->next)
        dir->next->prev = dir;
    volume->dirs = dir;
    return dir;
fail:
    free(dir);
    return NULL;
}

static int end_directory(void)
{
    ERROR(ENOENT, "no more entries");
    return -1;
}

int hfs_readdir2(hfsdir *dir, hfsdirent *entry, CatKeyRec *key, CatDataRec *record)
{
    if (!dir->dirid) {
        hfsvol *volume = hfs_mounts;
        while (volume && volume != dir->vptr)
            volume = volume->next;
        if (!volume)
            return end_directory();
        CatDataRec thread;
        node slot;
        if (ClassicHfsFindCatalogThread(volume, HFS_CNID_ROOTDIR, &thread, NULL, cdrThdRec) <= 0)
            return -1;
        int found = ClassicHfsFindCatalogEntry(volume, HFS_CNID_ROOTPAR, thread.u.dthd.thdCName,
                                               record, NULL, &slot);
        if (found < 0)
            return -1;
        if (!found || thread.u.dthd.thdParID != HFS_CNID_ROOTPAR || record->cdrType != cdrDirRec ||
            record->u.dir.dirDirID != HFS_CNID_ROOTDIR ||
            !ClassicHfsReadLeafEntry(slot.data, slot.roff, slot.nd.ndNRecs, slot.rnum, key, NULL)) {
            ERROR(EIO, "invalid classic HFS root directory entry");
            return -1;
        }
        ClassicHfsDescribeEntry(key->ckrParID, key->ckrCName, record, entry);
        dir->vptr = volume->next;
        return 0;
    }
    node *leaf = &dir->n;
    if (leaf->rnum < 0)
        return end_directory();
    for (;;) {
        ++leaf->rnum;
        while (leaf->rnum >= leaf->nd.ndNRecs) {
            unsigned long previous = leaf->nnum, next = leaf->nd.ndFLink;
            if (!next) {
                leaf->rnum = -1;
                return end_directory();
            }
            if (next == previous || dir->leavesVisited >= leaf->bt->hdr.bthNNodes)
                goto broken;
            leaf->nnum = next;
            if (ClassicHfsReadTreeNode(leaf) < 0) {
                leaf->rnum = -1;
                return -1;
            }
            if (leaf->nd.ndType != -1 || leaf->nd.ndNHeight != 1 || leaf->nd.ndBLink != previous)
                goto broken;
            ++dir->leavesVisited;
            leaf->rnum = 0;
        }
        if (!ClassicHfsReadLeafEntry(leaf->data, leaf->roff, leaf->nd.ndNRecs, leaf->rnum, key, record))
            goto broken;
        if (key->ckrParID != dir->dirid) {
            leaf->rnum = -1;
            return end_directory();
        }
        if (record->cdrType == cdrDirRec || record->cdrType == cdrFilRec) {
            ClassicHfsDescribeEntry(key->ckrParID, key->ckrCName, record, entry);
            return 0;
        }
        if (record->cdrType != cdrThdRec && record->cdrType != cdrFThdRec)
            goto broken;
    }
broken:
    leaf->rnum = -1;
    ERROR(EIO, "invalid classic HFS directory leaf or record");
    return -1;
}

int hfs_readdir(hfsdir *dir, hfsdirent *entry)
{
    CatKeyRec key;
    CatDataRec record;
    return hfs_readdir2(dir, entry, &key, &record);
}

int hfs_closedir(hfsdir *dir)
{
    if (dir->prev)
        dir->prev->next = dir->next;
    else
        dir->vol->dirs = dir->next;
    if (dir->next)
        dir->next->prev = dir->prev;
    free(dir);
    return 0;
}
