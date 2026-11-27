// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#undef ERROR
#include <errno.h>
#include "hfs/libhfs/internal.h"
#include "hfs/libhfs/block.h"
#include "hfs/libhfs/btree.h"
#include "hfs/ClassicHfsTreeFlush.h"
#include "hfs/ClassicHfsVolumeAllocation.h"
#include "hfs/libhfs/node.h"
#include "hfs/libhfs/file.h"
#include "hfs/libhfs/volume.h"
#include "hfs/ClassicHfsGrowTreeMap.h"
#include "hfs/ClassicHfsRecordBytes.h"
#include "hfs/ClassicHfsCatalogKey.h"
#include "hfs/ClassicHfsCatalogLeaf.h"
#include "hfs/ClassicHfsCatalogThread.h"
#include "hfs/ClassicHfsDirectory.h"
#include "hfs/ClassicHfsEntry.h"
#include "hfs/ClassicHfsCatalogFile.h"
#include "hfs/ClassicHfsExtents.h"
#include "hfs/ClassicHfsNodeIO.h"
#include "hfs/ClassicHfsGeometry.h"
#include "hfs/ClassicHfsBitmap.h"
#include "hfs/ClassicHfsTreeHeader.h"
#include "hfs/ClassicHfsTime.h"
#include "hfs/copyin.h"
#include "hfs/copyout.h"
#include "hfs/crc.h"
/* This transfer-format harness operates only on its own new image, without
   a recovery transaction. The release build links the real sector hooks. */
int hfv_hfs_recover_image(int fd, int writable) { return 0; }
int hfv_hfs_before_sector_write(int fd, unsigned long long sector) { return 0; }
void hfv_hfs_after_sector_write(int fd) {}

static void must(int ok,const char* action){if(!ok){fprintf(stderr,"FAIL: BinHex 5: %s (%s; %s; %s)\n",action,hfs_error?hfs_error:"",cpi_error,cpo_error);exit(1);}}
static void save(const char* path,const unsigned char* bytes,unsigned size){HANDLE f=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD wrote=0;must(f!=INVALID_HANDLE_VALUE,"create host fixture");must(WriteFile(f,bytes,size,&wrote,NULL)&&wrote==size,"write host fixture");CloseHandle(f);}
static unsigned char* load(const char* path,unsigned* size){FILE*f=fopen(path,"rb");unsigned char*b;must(f!=NULL,"open export");fseek(f,0,SEEK_END);*size=(unsigned)ftell(f);rewind(f);b=malloc(*size);must(b!=NULL&&fread(b,1,*size,f)==*size,"read export");fclose(f);return b;}
static unsigned char value(unsigned i,unsigned fork){return (unsigned char)((i*37)^(i>>3)^(fork*91));}
static void verify(hfsvol*v,char*name,unsigned d,unsigned r){hfsfile*f=hfs_open(v,name);unsigned fork;unsigned char b[5000];must(f!=NULL,"open imported forks");for(fork=0;fork<2;fork++){unsigned n=fork?r:d,i;must(hfs_setfork(f,fork)==0,"choose fork");must(hfs_read(f,b,n)==n,"read imported fork");for(i=0;i<n;i++)must(b[i]==value(i,fork),"fork byte fidelity");must(hfs_read(f,b,1)==0,"fork EOF");}must(hfs_close(f)==0,"close imported forks");}
#include "hfs/ClassicHfsSectorIO.h"
static hfv_offset_t test_sector_seek(int delta, hfv_offset_t offset, int origin)
{
 return offset + delta;
}
static int short_sector_write(int fd, const void *buffer, unsigned size)
{
 return size - 1;
}
#include "hfs/ClassicHfsCache.h"
typedef struct { unsigned count; unsigned long seen[3]; } CacheWriteLog;
static int record_cache_write(void *context, unsigned long sector, unsigned char (*bytes)[512])
{
 CacheWriteLog *log = context;
 log->seen[log->count++] = sector;
 return sector == 3 ? -1 : 0;
}

#include "hfs/ClassicHfsForkIO.h"
#include "hfs/ClassicHfsPath.h"
static int read_pattern_block(void *context, uint32_t sector, unsigned char (*bytes)[512])
{
 if (sector == *(uint32_t *)context) return -1;
 for (unsigned i = 0; i < 512; ++i) (*bytes)[i] = (unsigned char)(sector + i);
 return 0;
}

typedef struct { unsigned char bytes[512]; unsigned long capacity; } ForkWriteTest;
static int test_load_fork(void *context, uint32_t sector, unsigned char (*out)[512])
{
 memcpy(*out, ((ForkWriteTest *)context)->bytes, 512); return 0;
}
static int test_store_fork(void *context, uint32_t sector, unsigned char (*in)[512])
{
 if (sector != 0) return -1;
 memcpy(((ForkWriteTest *)context)->bytes, *in, 512); return 0;
}
static int test_grow_fork(void *context)
{
 ((ForkWriteTest *)context)->capacity += 512; return 0;
}

static int load_test_fork_record(void *context, unsigned first, ClassicHfsExtent (*record)[3])
{
 unsigned *calls = context;
 ++*calls;
 ClassicHfsExtent next[3] = {{20,1},{22,1},{24,1}};
 memcpy(*record, next, sizeof(next));
 return first == 3;
}
static int count_node_transfers(void *context, uint32_t number, unsigned char (*bytes)[512])
{
 ++*(int *)context;
 memset(*bytes, 0, 512);
 ClassicHfsWrite16(*bytes + 510, 14);
 return 0;
}
static int partial_node_transfer(void *context, uint32_t number, unsigned char (*bytes)[512])
{
 memset(*bytes, 0, 512);
 return *(int *)context;
}
static int compare_test_keys(unsigned char *left, unsigned char *right)
{
 return (int)left[1] - right[1];
}
static void verify_formatted_tree_headers(const char *image)
{
    unsigned char mdb[512], node[512];
    FILE *file = fopen(image, "rb");
    must(file && fseek(file, 1024, SEEK_SET) == 0 &&
         fread(mdb, 1, sizeof(mdb), file) == sizeof(mdb), "read formatted MDB");
    unsigned char alternate[512];
    must(fseek(file, -1024, SEEK_END) == 0 &&
         fread(alternate, 1, sizeof(alternate), file) == sizeof(alternate) &&
         !memcmp(mdb, alternate, sizeof(mdb)), "formatted primary and alternate MDB agree");
    must(ClassicHfsRead16(mdb) == 0x4244 && ClassicHfsRead16(mdb + 14) == 3 &&
         ClassicHfsRead32(mdb + 30) == 16 && mdb[36] == 3 && !memcmp(mdb + 37, "BH5", 3) &&
         ClassicHfsRead32(mdb + 84) == 0 && ClassicHfsRead32(mdb + 88) == 0 &&
         ClassicHfsRead16(mdb + 34) < ClassicHfsRead16(mdb + 18),
         "formatted volume name, root counters, next ID and free space");
    for (unsigned tree = 0; tree < 2; ++tree) {
        long offset = 512L * ClassicHfsRead16(mdb + 28) +
            ClassicHfsRead32(mdb + 20) * ClassicHfsRead16(mdb + (tree ? 150 : 134));
        must(fseek(file, offset, SEEK_SET) == 0 &&
             fread(node, 1, sizeof(node), file) == sizeof(node), "read formatted tree header");
        must(node[8] == 1 && ClassicHfsRead16(node + 10) == 3 &&
             ClassicHfsRead16(node + 32) == 512 && ClassicHfsRead16(node + 34) == (tree ? 37 : 7),
             "formatted tree type, record count and key length");
        must(ClassicHfsRead16(node + 510) == 14 && ClassicHfsRead16(node + 508) == 120 &&
             ClassicHfsRead16(node + 506) == 248 && ClassicHfsRead16(node + 504) == 504 &&
             (node[248] & 0x80), "formatted tree offsets and reserved header bit");
    }
    fclose(file);
}

// Corrupt only the new test image, before its first writable mount.
static void verify_repeated_tree_map(char *image)
{
    unsigned char mdb[512], original[1024], changed[1024];
    FILE *f = fopen(image, "r+b");
    must(f && fseek(f, 1024, SEEK_SET) == 0 && fread(mdb, 1, 512, f) == 512, "read test MDB");
    long tree = ClassicHfsRead16(mdb + 28) * 512L +
                ClassicHfsRead16(mdb + 134) * ClassicHfsRead32(mdb + 20);
    must(fseek(f, tree, SEEK_SET) == 0 && fread(original, 1, sizeof(original), f) == sizeof(original),
         "save extents header and following node");
    memcpy(changed, original, 512);
    memset(changed + 512, 0, 512);
    ClassicHfsWrite32(changed, 1);
    unsigned char *part = changed + 512;
    ClassicHfsWrite32(part, 1);
    part[8] = 2;
    ClassicHfsWrite16(part + 10, 1);
    ClassicHfsWrite16(part + 510, 14);
    ClassicHfsWrite16(part + 508, 506);
    must(fseek(f, tree, SEEK_SET) == 0 && fwrite(changed, 1, sizeof(changed), f) == sizeof(changed)
         && fclose(f) == 0, "install self-linked tree map");
    must(hfs_mount(image, 0, O_RDONLY) == NULL && hfs_error && strstr(hfs_error, "repeated"),
         "mount rejects repeated map node");
    f = fopen(image, "r+b");
    must(f && fseek(f, tree, SEEK_SET) == 0 && fwrite(original, 1, sizeof(original), f) == sizeof(original)
         && fclose(f) == 0, "restore disposable test image");
}

#include "hfs/ClassicHfsSectorUndo.h"
typedef struct { unsigned writes, reads, syncs; } UndoTrace;
static int undo_test_write(void *context, unsigned long sector, unsigned char (*bytes)[512])
{
    UndoTrace *trace = context;
    must(sector == 3 - trace->writes && (*bytes)[0] == sector, "reverse undo order and original bytes");
    ++trace->writes;
    return sector == 2 ? -1 : 0;
}
static int undo_test_read(void *context, unsigned long sector, unsigned char (*bytes)[512])
{
    ++((UndoTrace *)context)->reads;
    memset(*bytes, 0, 512);
    (*bytes)[0] = (unsigned char)sector;
    return 0;
}
static int undo_test_sync(void *context)
{
    ++((UndoTrace *)context)->syncs;
    return 0;
}

static int capture_test_read(void *context, unsigned long sector, unsigned char (*bytes)[512])
{
    unsigned *reads = context;
    ++*reads;
    memset(*bytes, (unsigned char)*reads, 512);
    return *reads == 1 ? -1 : 0;
}

int main(int argc,char**argv){
    {
        hfsvol volume = {0};
        hfsfile file = {0};
        file.vol = &volume;
        file.pos = 17;
        file.cat.u.fil.filLgLen = file.cat.u.fil.filPyLen = 512;
        must(hfs_setfork(&file, 1) < 0 && errno == EIO && hfs_getfork(&file) == 0 &&
             file.pos == 17, "invalid allocation size preserves selected fork and position");
    }
    {
        hfsdir dir = {0};
        hfsdirent entry;
        dir.dirid = HFS_CNID_ROOTDIR;
        dir.n.nnum = dir.n.nd.ndFLink = 3;
        must(hfs_readdir(&dir, &entry) < 0 && errno == EIO && dir.n.rnum == -1,
             "directory enumeration rejects a self-linked leaf before reading it again");
    }
    {
        hfsvol volume = {0};
        volume.flags = HFS_UPDATE_ALTMDB;
        volume.vlen = 4;
        must(ClassicHfsFlushVolumeMetadata(&volume, 1) < 0 && errno == EIO &&
             !(volume.mdb.drAtrb & HFS_ATRB_UMOUNTED) && (volume.flags & HFS_UPDATE_MDB),
             "failed alternate header preflight keeps volume unclean and pending for retry");
    }
    {
        hfsvol volume = {0};
        block bitmap = {0x01}, original;
        memcpy(original, bitmap, sizeof(bitmap));
        volume.vbm = &bitmap;
        volume.mdb.drNmAlBlks = 8;
        volume.mdb.drFreeBks = 7;
        volume.mdb.drXTExtRec[0].xdrNumABlks = 1;
        volume.mdb.drCTExtRec[0].xdrStABN = 8;
        volume.mdb.drCTExtRec[0].xdrNumABlks = 1;
        must(ClassicHfsRecoverAllocationBitmap(&volume) < 0 && volume.vbm == &bitmap &&
             !memcmp(bitmap, original, sizeof(bitmap)) && !volume.flags && volume.mdb.drFreeBks == 7,
             "failed catalog extent check discards earlier overflow extent bitmap repairs");
    }
    {
        hfsvol volume = {0};
        volume.mdb.drNxtCNID = 15;
        must(ClassicHfsCreateDirectoryRecords(&volume, HFS_CNID_ROOTDIR, "Rejected") < 0 &&
             errno == EIO && !volume.flags, "reserved folder ID rejected before tree reservation");
        volume.mdb.drNxtCNID = UINT32_MAX;
        must(ClassicHfsCreateDirectoryRecords(&volume, HFS_CNID_ROOTDIR, "Rejected") < 0 &&
             errno == ENOSPC && volume.mdb.drNxtCNID == UINT32_MAX && !volume.flags,
             "exhausted catalog ID cannot wrap into reserved IDs");
        volume.mdb.drNxtCNID = 0x80000000u;
        must(ClassicHfsCheckNewCatalogId(&volume, 0) == 0, "high-bit catalog IDs remain available");
    }
    {
        hfsvol volume = {0};
        must(ClassicHfsAdjustEntryCount(&volume, HFS_CNID_ROOTPAR, 1, 1) == 0 &&
             !volume.mdb.drDirCnt, "root creation is excluded from volume directory count");
        volume.mdb.drFilCnt = 7;
        must(ClassicHfsAdjustEntryCount(&volume, HFS_CNID_ROOTDIR, 0, -1) < 0 &&
             volume.mdb.drFilCnt == 7 && !volume.flags, "root count underflow preserves volume count");
        volume.mdb.drNmFls = UINT16_MAX;
        must(ClassicHfsAdjustEntryCount(&volume, HFS_CNID_ROOTDIR, 0, 1) < 0 &&
             volume.mdb.drNmFls == UINT16_MAX && volume.mdb.drFilCnt == 7 && !volume.flags,
             "root count overflow is rejected before catalog access");
    }
    {
        hfsvol volume = {0};
        node cursor = {0}, original;
        CatDataRec record = {0};
        cursor.bt = &volume.ext;
        volume.ext.f.vol = &volume;
        cursor.nd.ndType = -1;
        cursor.nd.ndNHeight = 1;
        original = cursor;
        must(ClassicHfsStoreCatalogEntry(&record, &cursor) < 0 && errno == EIO &&
             !memcmp(cursor.data, original.data, sizeof(cursor.data)),
             "reject catalog update through an extents-tree cursor");
        cursor.bt = &volume.cat;
        volume.cat.f.vol = &volume;
        cursor.nd.ndType = 0;
        must(ClassicHfsStoreCatalogEntry(&record, &cursor) < 0 && errno == EIO,
             "reject catalog update through an index cursor");
    }
    {
        hfsvol volume = {0};
        hfsfile file = {0};
        block bitmap = {0x80};
        volume.vbm = &bitmap;
        volume.mdb.drAlBlkSiz = 512;
        volume.mdb.drNmAlBlks = 4;
        file.vol = &volume;
        file.cat.u.fil.filPyLen = 1024;
        file.cat.u.fil.filExtRec[0].xdrNumABlks = 2;
        must(ClassicHfsTrimForkAllocation(&file) < 0 && errno == EIO &&
             bitmap[0] == 0x80 && file.cat.u.fil.filPyLen == 1024 &&
             file.cat.u.fil.filExtRec[0].xdrNumABlks == 2 && !volume.flags,
             "invalid fork allocation rejected before releasing any sector");
    }
    {
        ClassicHfsTreeHeader header = {0};
        header.bthRoot = header.bthFNode = header.bthLNode = 1;
        header.bthNNodes = 4; header.bthDepth = 1; header.bthNRecs = 3;
        must(ClassicHfsValidMutationRoot(&header, 0, 0, 3) &&
             !ClassicHfsValidMutationRoot(&header, 0, 0, 2) &&
             !ClassicHfsValidMutationRoot(&header, 2, 0, 3), "validate record totals and root sibling links");
        header.bthDepth = 2; header.bthRoot = 2; header.bthLNode = 3;
        must(ClassicHfsValidMutationRoot(&header, 0, 0, 2), "accept bounded index root");
        header.bthLNode = header.bthNNodes;
        must(!ClassicHfsValidMutationRoot(&header, 0, 0, 2), "reject out-of-range leaf endpoint before mutation");
    }
    {
        hfsvol volume = {0};
        node child = {0};
        unsigned char dest[42], original[42];
        memset(dest, 0xaa, sizeof(dest)); memcpy(original, dest, sizeof(dest));
        child.bt = &volume.cat; volume.cat.f.vol = &volume;
        child.nnum = 4; child.nd.ndNRecs = 1;
        child.roff[0] = 14; child.roff[1] = 24; child.data[14] = 6;
        must(ClassicHfsBuildSeparator(&child, dest, 41, NULL) < 0 &&
             !memcmp(dest, original, sizeof(dest)), "short parent slot preserves adjacent separator bytes");
        child.roff[1] = 12;
        must(ClassicHfsBuildSeparator(&child, dest, sizeof(dest), NULL) < 0 &&
             !memcmp(dest, original, sizeof(dest)), "invalid child offsets reject separator publication");
    }
    {
        ClassicHfsSectorUndo undo = {0};
        unsigned reads = 0;
        must(ClassicHfsCaptureSector(&undo, 0, &reads, capture_test_read) == ClassicHfsCaptureReadFailed &&
             undo.count == 0, "failed capture never publishes partial bytes");
        must(ClassicHfsCaptureSector(&undo, 0, &reads, capture_test_read) == ClassicHfsCaptured &&
             ClassicHfsCaptureSector(&undo, 2049, &reads, capture_test_read) == ClassicHfsCaptured &&
             ClassicHfsCaptureSector(&undo, 0, &reads, capture_test_read) == ClassicHfsCaptured &&
             undo.count == 2 && reads == 3 && undo.records[0].before[511] == 2 &&
             undo.records[1].before[511] == 3, "repeated sector keeps original bytes across hash collision");
        free(undo.records);
    }
    {
        ClassicHfsUndoRecord records[3] = {0};
        UndoTrace trace = {0};
        for (unsigned i = 0; i < 3; ++i)
            records[i].before[0] = (unsigned char)(records[i].sector = i + 1);
        must(ClassicHfsRestoreSectors(records, 0, &trace, undo_test_write, undo_test_read, undo_test_sync) == 0 &&
             !trace.syncs, "empty rollback leaves storage untouched");
        must(ClassicHfsRestoreSectors(records, 3, &trace, undo_test_write, undo_test_read, undo_test_sync) < 0 &&
             trace.writes == 3 && trace.reads == 2 && trace.syncs == 1, "undo continues and syncs after failed restoration");
    }
    {
        ClassicHfsCache *cache = NULL;
        CacheWriteLog log = {0};
        must(ClassicHfsAcquireCache(&cache) == 0, "allocate cache");
        cache->chain[0].bnum = 3;
        cache->chain[0].flags = ClassicHfsCacheValid | ClassicHfsCacheDirty;
        must(ClassicHfsAcquireCache(&cache) == 0 &&
             ClassicHfsDrainCache(&cache, 0, record_cache_write, &log) < 0 &&
             (cache->chain[0].flags & ClassicHfsCacheDirty), "flush failure retains pending sector");
        must(ClassicHfsDrainCache(&cache, 1, record_cache_write, &log) < 0 && !cache &&
             log.count == 2 && log.seen[1] == 3, "close reports final failure and releases cache");
    }
    {
        hfsvol volume = {0};
        block sector = {0}, bitmap = {0x80};
        volume.flags = HFS_UPDATE_ALTMDB | HFS_UPDATE_MDB;
        volume.vlen = 4;
        volume.cat.f.clump = 4096;
        must(ClassicHfsEnableVolumeCache(&volume) == 0 &&
             ClassicHfsStoreVolumeHeader(&volume) < 0 && errno == EIO &&
             volume.mdb.drCTClpSiz == 0 && volume.flags == (HFS_UPDATE_ALTMDB | HFS_UPDATE_MDB) &&
             ClassicHfsCloseVolumeCache(&volume) == 0, "overlapping master directories reject before staging sectors");
        volume.flags = 0;
        volume.lpa = 1; volume.vlen = 8;
        {
            volume.pnum = 1;
            must(ClassicHfsEnableVolumeCache(&volume) == 0, "allocate descriptor fixture cache");
            ClassicHfsWrite16(sector, 0x4552);
            ClassicHfsWrite16(sector + 2, 512);
            must(ClassicHfsWriteSector(&volume, 0, &sector) == 0 &&
                 ClassicHfsLocateVolume(&volume) < 0 && errno == EINVAL &&
                 volume.vlen == 8 && volume.pnum == 1, "empty descriptor does not enable an unbounded map scan");
            ClassicHfsWrite16(sector, 0x4c4b);
            must(ClassicHfsWriteSector(&volume, 0, &sector) == 0 &&
                 ClassicHfsLocateVolume(&volume) == 0 && !volume.pnum && volume.vlen == 8,
                 "bootable floppy selects whole volume");
            memset(sector, 0, sizeof(sector));
            ClassicHfsWrite16(sector, 0x504d);
            ClassicHfsWrite32(sector + 4, 2);
            ClassicHfsWrite32(sector + 8, 3);
            ClassicHfsWrite32(sector + 12, 3);
            memcpy(sector + 48, "Apple_HFS", 9);
            volume.pnum = 1;
            must(ClassicHfsWriteSector(&volume, 1, &sector) == 0, "stage first partition");
            ClassicHfsWrite32(sector + 8, 5);
            must(ClassicHfsWriteSector(&volume, 2, &sector) == 0 &&
                 ClassicHfsSelectPartition(&volume) < 0 && !volume.vstart && volume.vlen == 8,
                 "reject later overlapping partition before publishing geometry");
            ClassicHfsWrite32(sector + 8, 6);
            ClassicHfsWrite32(sector + 12, 2);
            volume.pnum = 2;
            must(ClassicHfsWriteSector(&volume, 2, &sector) == 0 &&
                 ClassicHfsSelectPartition(&volume) == 0 && volume.vstart == 6 && volume.vlen == 2,
                 "select second HFS partition ending at disk boundary");
            volume.vstart = 0; volume.vlen = 8;
            free(volume.cache); // Discard the synthetic sectors without device I/O.
            volume.cache = NULL;
        }
        volume.flags = HFS_READONLY;
        must(ClassicHfsStoreRawSector(&volume, 0, &sector) < 0 && errno == EROFS,
             "raw sector write rejects a read-only volume before accessing storage");
        volume.flags = 0;
        {
            bcache before;
            must(ClassicHfsEnableVolumeCache(&volume) == 0, "initialize logical sector cache");
            memcpy(&before, volume.cache, sizeof(before));
            must(ClassicHfsWriteSector(&volume, 8, &sector) < 0 && errno == EIO &&
                 ClassicHfsReadSector(&volume, 8, &sector) < 0 && errno == EIO &&
                 !memcmp(&before, volume.cache, sizeof(before)), "reject outside sector before cache lookup");
            must(ClassicHfsCloseVolumeCache(&volume) == 0, "rejected sector leaves nothing to flush");
        }
        volume.mdb.drAlBlSt = 4; volume.mdb.drNmAlBlks = 1;
        volume.mdb.drAtrb = HFS_ATRB_UMOUNTED;
        must(ClassicHfsTransferAllocationSector(&volume, 0, 0, &sector, 1) < 0 && errno == EIO &&
             !volume.flags && !volume.mdb.drWrCnt, "missing allocation bitmap rejects writes");
        volume.vbm = &bitmap;
        volume.flags = HFS_WRITE_FAILED;
        must(ClassicHfsTransferAllocationSector(&volume, 0, 0, &sector, 1) < 0 &&
             volume.mdb.drAtrb == HFS_ATRB_UMOUNTED && !volume.mdb.drLsMod &&
             !volume.mdb.drWrCnt && volume.flags == HFS_WRITE_FAILED,
             "failed sector submission preserves master directory metadata");
    }
    {
        char map = (char)0xc0;
        btree tree = {0};
        node cursor;
        tree.map = &map;
        tree.mapsz = 1;
        tree.hdr.bthNNodes = 2;
        tree.hdr.bthFree = 1;
        ClassicHfsInitTreeNode(&cursor, &tree, ndLeafNode, 1);
        must(ClassicHfsAllocateTreeNode(&cursor) < 0 && cursor.nnum == UINT32_MAX &&
             tree.hdr.bthFree == 1 && !tree.flags && (unsigned char)map == 0xc0,
             "failed allocation preserves tree bitmap, counters and unassigned cursor");
        map = (char)0x80;
        must(ClassicHfsAllocateTreeNode(&cursor) == 0 && cursor.nnum == 1 &&
             ClassicHfsAllocateTreeNode(&cursor) < 0 && !tree.hdr.bthFree &&
             (unsigned char)map == 0xc0, "claim a tree node only once per cursor");
    }
    {
        node cursor = {0};
        unsigned char record[] = {1, 2};
        int length = sizeof(record);
        cursor.rnum = 1;
        cursor.roff[0] = 14;
        must(ClassicHfsInsertAtCursor(&cursor, record, &length) < 0 && !cursor.nd.ndNRecs &&
             cursor.roff[0] == 14 && record[0] == 1 && length == 2,
             "invalid insertion slot preserves caller node and incoming record");
    }
    for (int slot = 0; slot < 3; ++slot) {
        unsigned char sector[512] = {0};
        uint16_t offsets[] = {14, 18, 22, 26}, count = 3;
        sector[14] = 1; sector[18] = 2; sector[22] = 3;
        must(!ClassicHfsRemoveNodeRecord(sector, offsets, &count, 3) && count == 3 &&
             offsets[3] == 26 && sector[22] == 3, "reject removal past the last node slot");
        must(ClassicHfsRemoveNodeRecord(sector, offsets, &count, slot) && count == 2 &&
             offsets[0] == 14 && offsets[1] == 18 && offsets[2] == 22 &&
             sector[14] == (slot == 0 ? 2 : 1) && sector[18] == (slot == 2 ? 2 : 3),
             "first, middle and last node deletions preserve the surviving records");
    }
    {
        ClassicHfsNodeDescriptor child = {0};
        child.ndNHeight = 1; child.ndType = -1; child.ndNRecs = 1;
        must(ClassicHfsPromotableChild(&child, 2, 7, 7, 7) &&
             !ClassicHfsPromotableChild(&child, 2, 7, 7, 9) &&
             !ClassicHfsPromotableChild(&child, 3, 7, 3, 9), "root promotion checks leaf identity and height");
        child.ndFLink = 9;
        must(!ClassicHfsPromotableChild(&child, 2, 7, 7, 7), "root promotion rejects a linked child");
    }
    {
        hfsvol volume = {0};
        block bitmap = {0};
        ExtDescriptor extent = {7, 1};
        volume.vbm = &bitmap;
        volume.mdb.drNmAlBlks = 8;
        must(ClassicHfsClaimVolumeExtent(&volume, &extent) < 0 && errno == EIO &&
             !bitmap[0] && extent.xdrStABN == 7 && !volume.flags, "reject false full-volume count");
        bitmap[0] = 0xff;
        must(ClassicHfsClaimVolumeExtent(&volume, &extent) < 0 && errno == ENOSPC,
             "report full bitmap as out of space");
        volume.flags = HFS_WRITE_FAILED;
        must(ClassicHfsReleaseVolumeExtent(&volume, &extent) < 0 && bitmap[0] == 0xff &&
             !volume.mdb.drFreeBks && volume.flags == HFS_WRITE_FAILED, "quarantined volume retains extent");
    }
    {
        ClassicHfsTreeHeader header = {0};
        must(ClassicHfsCheckSearchRoot(&header) == 0, "empty classic tree lookup");
        header.bthNRecs = 1;
        must(ClassicHfsCheckSearchRoot(&header) < 0, "missing root with live records");
        header.bthRoot = 3; header.bthDepth = 2; header.bthNNodes = 3;
        must(ClassicHfsCheckSearchRoot(&header) < 0, "root outside tree");
        header.bthNNodes = 4;
        must(ClassicHfsCheckSearchRoot(&header) == 1, "in-range lookup root");
        header.bthDepth = 4;
        must(ClassicHfsCheckSearchRoot(&header) < 0, "lookup depth exceeds available nonheader nodes");
    }
    {
        unsigned char sector[512] = {0}, before[512];
        uint16_t offsets[] = {14, 24};
        ClassicHfsCatalogRecord record = {0};
        record.cdrType = cdrFilRec;
        int key = ClassicHfsWriteCatalogKey(sector + 14, 2, "X");
        sector[14 + key] = cdrFilRec;
        memcpy(before, sector, sizeof(before));
        must(!ClassicHfsReadLeafEntry(sector, offsets, 1, 0, NULL, &record) &&
             !ClassicHfsUpdateLeafEntry(sector, offsets, 1, 0, &record) &&
             !ClassicHfsUpdateLeafEntry(sector, offsets, 1, -1, &record) &&
             !memcmp(before, sector, sizeof(before)), "reject truncated catalog leaf and invalid slot before update");
    }
    {
        ClassicHfsCatalogRecord disk = {0}, opened = {0}, staged = {0};
        opened.cdrType = disk.cdrType = cdrFilRec;
        opened.u.fil.filFlNum = 20;
        disk.u.fil.filFlNum = 21;
        must(!ClassicHfsPrepareFileUpdate(&disk, &opened, 512, &staged) && staged.cdrType == 0,
             "reject a replacement file at the open name");
        disk.u.fil.filFlNum = 20;
        disk.cdrType = cdrDirRec;
        must(!ClassicHfsPrepareFileUpdate(&disk, &opened, 512, &staged), "reject a replacement directory");
        disk.cdrType = cdrFilRec;
        opened.u.fil.filExtRec[0].xdrStABN = 3;
        opened.u.fil.filRExtRec[0].xdrStABN = 9;
        must(ClassicHfsPrepareFileUpdate(&disk, &opened, 512, &staged) &&
             staged.u.fil.filStBlk == 3 && staged.u.fil.filRStBlk == 9 &&
             staged.u.fil.filClpSize == 512 && opened.u.fil.filStBlk == 0,
             "stage both fork starts without changing the open file");
    }
    ClassicHfsFormatGeometry geometry;
    must(!ClassicHfsPlanFormat(1599, &geometry) && ClassicHfsPlanFormat(1600, &geometry) &&
         geometry.allocationBlocks == 1594 && geometry.firstAllocationSector == 4,
         "minimum classic format geometry");
    must(ClassicHfsPlanFormat(65535, &geometry) && geometry.sectorsPerBlock == 1 &&
         geometry.allocationBlocks == 65514 && ClassicHfsPlanFormat(65536, &geometry) &&
         geometry.sectorsPerBlock == 2 && geometry.firstAllocationSector == 11 &&
         geometry.allocationBlocks == 32761, "classic format allocation-size transition");
    must(ClassicHfsPlanFormat(UINT32_MAX, &geometry) && geometry.blockBytes == 33554432 &&
         geometry.treeClumpBytes == 127u * geometry.blockBytes &&
         geometry.allocationBlocks == 65535, "classic format clump stays aligned and representable");
    must(ClassicHfsBitmapSectors(4096) == 1 && ClassicHfsBitmapSectors(4097) == 2 &&
         ClassicHfsBitmapSectors(65535) == 16, "bitmap sector rounding");
    must(ClassicHfsBitmapFits(3, 2, 5, 100) && !ClassicHfsBitmapFits(2, 1, 5, 100) &&
         !ClassicHfsBitmapFits(3, 2, 4, 100) && !ClassicHfsBitmapFits(3, 2, 5, 4) &&
         !ClassicHfsBitmapFits(3, 0, 5, 100), "bitmap reserved and truncated ranges");
 char image[1024],path[1024],badpath[1024],source[]="BH5:Source",target[]="BH5:Imported",label[]="BH5";
 HANDLE host;LARGE_INTEGER length;hfsvol*v;unsigned cases[][2]={{0,0},{1,0},{0,1},{127,129},{128,128},{129,127},{4097,3073}};unsigned k,fork;
 {
  unsigned char bytes[8] = {0x55, 0, 0, 0, 0, 0, 0, 0xaa};
  ClassicHfsWrite32(bytes + 1, UINT32_C(0x80fe01ff));
  ClassicHfsWrite16(bytes + 5, UINT16_C(0xfedc));
  must(memcmp(bytes, "\x55\x80\xfe\x01\xff\xfe\xdc\xaa", 8) == 0, "unaligned big-endian writes");
  must(ClassicHfsRead32(bytes + 1) == UINT32_C(0x80fe01ff) &&
       ClassicHfsRead16(bytes + 5) == UINT16_C(0xfedc), "high-bit big-endian reads");
 }
 {
  unsigned char fields[10] = {0};
  unsigned char *cursor = fields;
  char name[4];
  ClassicHfsAppendName(&cursor, "Mac", 4);
  ClassicHfsAppendName(&cursor, "X", 4);
  ClassicHfsAppend16(&cursor, 0xfedc);
  must(cursor == fields + sizeof(fields) && !memcmp(fields, "\x03Mac\x01X\0\0\xfe\xdc", 10),
       "fixed Pascal fields and padding");
  fields[0] = 0xff;
  cursor = fields;
  ClassicHfsTakeName(&cursor, name, sizeof(name));
  must(!name[0] && cursor == fields + 4, "invalid name consumes its field");
  ClassicHfsTakeName(&cursor, name, sizeof(name));
  must(!strcmp(name, "X") && ClassicHfsTake16(&cursor) == 0xfedc, "following fields stay aligned");
 }
 {
  time_t now = time(NULL);
  struct tm utc = *gmtime(&now);
  utc.tm_isdst = localtime(&now)->tm_isdst;
  uint32_t epoch = (uint32_t)(INT64_C(2082844800) + now - mktime(&utc));
  must(ClassicHfsFromUnixSeconds(0) == epoch && ClassicHfsToUnixSeconds(epoch) == 0,
       "classic local epoch compatibility");
  must(ClassicHfsFromUnixSeconds(ClassicHfsToUnixSeconds(UINT32_MAX)) == UINT32_MAX &&
       ClassicHfsFromUnixSeconds(ClassicHfsToUnixSeconds(0)) == 0, "classic date limits");
 }
 {
  unsigned char first[8], last[8];
  ClassicHfsWriteExtentKey(first, 0xff, 1, 0xffff);
  ClassicHfsWriteExtentKey(last, 0, UINT32_MAX, 0);
  must(!memcmp(first, "\x07\xff\0\0\0\x01\xff\xff", 8), "extent key disk layout");
  must(ClassicHfsCompareExtentKeys(first, last) < 0 && ClassicHfsCompareExtentKeys(last, first) > 0,
       "unsigned file IDs take precedence over fork and block");
  ClassicHfsWriteExtentKey(last, 0, 1, 0xffff);
  must(ClassicHfsCompareExtentKeys(last, first) < 0, "data fork precedes resource fork");
  ClassicHfsWriteExtentKey(last, 0xff, 1, 0x8000);
  must(ClassicHfsCompareExtentKeys(last, first) < 0 && ClassicHfsCompareExtentKeys(first, first) == 0,
       "extent block order and equality");
 }
 {
  unsigned char bytes[39];
  ClassicHfsCatalogKey decoded;
  const char full[] = "1234567890123456789012345678901";
  memset(bytes, 0xcc, sizeof(bytes));
  must(ClassicHfsWriteCatalogKey(bytes, 42, full) == 38 && bytes[38] == 0xcc &&
       ClassicHfsReadCatalogKey(bytes, 38, &decoded) && decoded.ckrParID == 42 &&
       !strcmp(decoded.ckrCName, full), "maximum catalog key round trip");
  must(!ClassicHfsReadCatalogKey(bytes, 37, &decoded), "truncated catalog key");
  must(ClassicHfsWriteCatalogKey(bytes, 42, "12345678901234567890123456789012") < 0 &&
       bytes[0] == 37, "long name rejected before encoding");
  must(ClassicHfsWriteCatalogKey(bytes, 1, "") == 8 && bytes[7] == 0 &&
       ClassicHfsReadCatalogKey(bytes, 8, &decoded) && !decoded.ckrCName[0], "thread key padding");
  unsigned char thread[] = {6, 0, 0, 0, 0, 1, 0};
  unsigned char leaf[] = {7, 0, 0, 0, 0, 1, 1, 0x80};
  unsigned char index[38] = {37, 0, 0, 0, 0, 1, 1, 0x8a};
  must(ClassicHfsCompareCatalogKeys(thread, leaf) < 0 &&
       ClassicHfsCompareCatalogKeys(leaf, index) == 0, "thread, leaf and padded index collation");
  ClassicHfsWrite32(index + 2, UINT32_MAX);
  must(ClassicHfsCompareCatalogKeys(index, leaf) > 0 && ClassicHfsCompareCatalogKeys(leaf, index) < 0,
       "catalog parent IDs compare unsigned");
  leaf[6] = 31;
  must(ClassicHfsCatalogNameLength(leaf) == 0, "name cannot extend beyond its key");
 }
 {
  ClassicHfsExtent runs[3] = {{0xfedc, 2}, {0, 0}, {0x8000, 0xffff}}, decoded[3];
  unsigned char bytes[21] = {7};
  bytes[20] = 0x55;
  must(ClassicHfsEncodeExtents(bytes + 8, runs) == 12 &&
       !memcmp(bytes + 8, "\xfe\xdc\0\2\0\0\0\0\x80\0\xff\xff", 12) && bytes[20] == 0x55,
       "three extent descriptors on disk");
  ClassicHfsDecodeExtents(bytes + 8, decoded);
  must(!memcmp(decoded, runs, sizeof(runs)), "extent descriptor round trip");
  must(ClassicHfsValidExtentRecord(bytes, 20) && !ClassicHfsValidExtentRecord(bytes, 19),
       "overflow extent record boundary");
  bytes[0] = 6;
  must(!ClassicHfsValidExtentRecord(bytes, 20), "invalid overflow extent key length");
 }
 {
  unsigned char bytes[ClassicHfsNodeSize];
  ClassicHfsNodeDescriptor header = {0xabcdef01, 3, -1, 1, 2, -2}, decoded;
  uint16_t offsets[ClassicHfsMaxRecords+1] = {14, 32, 506}, readback[ClassicHfsMaxRecords+1];
  memset(bytes, 0x55, sizeof(bytes));
  must(ClassicHfsEncodeNode(bytes, &header, offsets) && bytes[14] == 0x55 &&
       ClassicHfsRead16(bytes + 506) == 506 && ClassicHfsRead16(bytes + 510) == 14,
       "reverse node offsets leave payload intact");
  must(ClassicHfsDecodeNode(bytes, &decoded, readback) && decoded.ndType == -1 &&
       decoded.ndFLink == header.ndFLink && decoded.ndResv2 == -2 &&
       !memcmp(offsets, readback, 6), "classic node descriptor round trip");
  ClassicHfsWrite16(bytes + 508, 12);
  must(!ClassicHfsDecodeNode(bytes, &decoded, readback), "reject descending node offsets");
  offsets[2] = 508;
  must(!ClassicHfsEncodeNode(bytes, &header, offsets) && ClassicHfsRead16(bytes + 508) == 12,
       "reject overlap with offset table before modifying node");
 }
 {
  unsigned char bytes[512] = {0};
  uint16_t offsets[] = {14, 16, 18, 24}, count = 3;
  memcpy(bytes + 18, "abcdef", 6);
  offsets[2] = 17;
  must(!ClassicHfsCompactNode(bytes, offsets, &count) && count == 3 &&
       offsets[1] == 16 && !memcmp(bytes + 18, "abcdef", 6), "reject malformed compaction");
  offsets[2] = 18;
  must(ClassicHfsCompactNode(bytes, offsets, &count) && count == 1 && offsets[0] == 14 &&
       offsets[1] == 20 && !memcmp(bytes + 14, "abcdef", 6), "compact overlapping live record");
  bytes[14] = 0;
  must(ClassicHfsCompactNode(bytes, offsets, &count) && !count && offsets[0] == 14,
       "compact fully deleted node");
 }
 {
  unsigned char bytes[512] = {0}, record[494] = {1, 2};
  uint16_t offsets[ClassicHfsMaxRecords + 1] = {14}, count = 0;
  must(ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 490),
       "insert large record in empty node");
  must(!ClassicHfsInsertNodeRecord(bytes, offsets, &count, 2, record, 2) &&
       !ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 4) &&
       count == 1 && offsets[1] == 504 && bytes[14] == 1,
       "reject invalid slot and offset-table overlap before insertion");
  record[0] = 3;
  must(ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 2) &&
       count == 2 && offsets[1] == 16 && offsets[2] == 506 &&
       bytes[14] == 3 && bytes[16] == 1, "insert before large record at exact capacity");
 }
 {
  unsigned char left[512] = {0}, right[512] = {0};
  uint16_t offsets[ClassicHfsMaxRecords + 1] = {14, 502};
  uint16_t sibling[] = {18, 20, 22}, count = 1;
  memcpy(right + 18, "abcd", 4);
  must(!ClassicHfsMergeNodeRecords(left, offsets, &count, right, sibling, 2) &&
       left[502] == 0 && count == 1 && offsets[2] == 0, "oversized merge preserves left node");
  offsets[1] = 500;
  must(ClassicHfsMergeNodeRecords(left, offsets, &count, right, sibling, 2) &&
       count == 3 && offsets[2] == 502 && offsets[3] == 504 &&
       !memcmp(left + 500, "abcd", 4) && !memcmp(right + 18, "abcd", 4),
       "exact capacity merge rebases sibling offsets and retains record order");
 }
 {
  unsigned char map[] = {0xff, 0x40};
  must(!ClassicHfsClaimTreeNode(map, 1, 10, 1) && map[1] == 0x40,
       "tree allocation stays within installed map");
  must(ClassicHfsClaimTreeNode(map, 2, 10, 1) == 8 && map[1] == 0xc0,
       "tree allocation reaches next map byte");
  must(!ClassicHfsCanReleaseTreeNode(map, 2, 10, 9, 8) &&
       ClassicHfsCanReleaseTreeNode(map, 2, 10, 0, 8) && ClassicHfsBitIsSet(map, 8),
       "merge preflight validates free count without releasing its right sibling");
  must(!ClassicHfsReleaseTreeNode(map, 2, 10, 0, 0) &&
       ClassicHfsReleaseTreeNode(map, 2, 10, 0, 8) &&
       !ClassicHfsReleaseTreeNode(map, 2, 10, 1, 8) && map[1] == 0x40,
       "protect tree header and reject double release");
  map[0] = 0;
  must(!ClassicHfsClaimTreeNode(map, 2, 10, 2) && map[0] == 0,
       "reject missing tree header bit");
 }
 {
  unsigned char bytes[512] = {0}, key[2] = {1, 0};
  uint16_t offsets[9];
  for (int i = 0; i <= 8; ++i) offsets[i] = (uint16_t)(14 + i * 2);
  for (unsigned mask = 0; mask < 256; ++mask) {
   for (int i = 0; i < 8; ++i) {
    bytes[offsets[i]] = (unsigned char)((mask >> i) & 1);
    bytes[offsets[i] + 1] = (unsigned char)(2 + 2 * (i / 2));
   }
   for (key[1] = 0; key[1] <= 10; ++key[1]) {
    int expected = -1, observed = -2;
    for (int i = 0; i < 8; ++i)
     if (bytes[offsets[i]] && bytes[offsets[i] + 1] <= key[1]) expected = i;
    int exact = ClassicHfsSearchNode(bytes, offsets, 8, key, compare_test_keys, &observed);
    must(observed == expected && exact == (expected >= 0 && bytes[offsets[expected] + 1] == key[1]),
         "node lookup preserves last live predecessor and exact match");
   }
  }
 }
 {
  unsigned char bytes[512] = {0}, key[8] = {7};
  ClassicHfsNodeDescriptor node = {0};
  uint16_t offsets[] = {14, 26};
  uint32_t child = 0;
  int slot;
  node.ndNHeight = 2; node.ndNRecs = 1;
  bytes[14] = 7; bytes[25] = 3;
  must(ClassicHfsLookupLevel(bytes, &node, offsets, 2, 0, key, compare_test_keys, &slot, 4, &child, ClassicHfsLookup)
       == ClassicHfsVisitChild && child == 3, "follow validated index child");
  must(ClassicHfsLookupLevel(bytes, &node, offsets, 1, 0, key, compare_test_keys, &slot, 4, &child, ClassicHfsLookup)
       == ClassicHfsInvalidSearch, "reject repeated height on cyclic descent");
  offsets[1] = 24;
  must(ClassicHfsLookupLevel(bytes, &node, offsets, 2, 0, key, compare_test_keys, &slot, 4, &child, ClassicHfsLookup)
       == ClassicHfsInvalidSearch, "reject truncated child pointer");
  offsets[1] = 26;
  bytes[15] = 2;
  must(ClassicHfsLookupLevel(bytes, &node, offsets, 2, 0, key, compare_test_keys,
       &slot, 4, &child, ClassicHfsInsert) == ClassicHfsVisitChild && slot == -1 && child == 3,
       "insert below first separator descends through leftmost child");
  must(ClassicHfsLookupLevel(bytes, &node, offsets, 2, 0, key, compare_test_keys,
       &slot, 4, &child, ClassicHfsDelete) == ClassicHfsMissingKey,
       "delete below first separator stops without selecting a child");
 }
 {
  unsigned char buffer[48] = {0};
  unsigned char *key = buffer + 2;
  key[0] = 7; key[5] = 9; key[6] = 1; key[7] = 'Z';
  must(!ClassicHfsWriteIndexRecord(buffer, 41, key, 8, 1, 4) &&
       !ClassicHfsWriteIndexRecord(buffer, 42, key, 7, 1, 4) && key[7] == 'Z',
       "reject short separator source and destination before encoding");
  must(ClassicHfsWriteIndexRecord(buffer, 42, key, 8, 1, 4) == 42 &&
       buffer[0] == 37 && buffer[5] == 9 && buffer[7] == 'Z' &&
       buffer[8] == 0 && ClassicHfsRead32(buffer + 38) == 4,
       "encode overlapping catalog separator with fixed padding");
  buffer[0] = 7;
  must(ClassicHfsWriteIndexRecord(buffer, 12, buffer, 8, 0, 8) == 12 &&
       ClassicHfsRead32(buffer + 8) == 8, "encode extent separator in place");
 }
 {
  unsigned char left[512] = {0}, right[512] = {0}, record[200] = {1};
  uint16_t offsets[36] = {14, 214, 414, 416, 418}, other[36], count = 4, sibling = 0;
  left[14] = 2; left[214] = 3; left[414] = 4; left[416] = 5;
  must(ClassicHfsSplitNodeRecords(left, offsets, &count, right, other, &sibling, 0, record, 200) &&
       count == 2 && sibling == 3 && left[offsets[0]] == 1 && left[offsets[1]] == 2 &&
       right[other[0]] == 3 && right[other[2]] == 5, "split uneven records by occupied bytes");
 }
 {
  ClassicHfsNodeDescriptor previous = {0}, next = {0};
  previous.ndType = next.ndType = -1;
  previous.ndNHeight = next.ndNHeight = 1;
  next.ndBLink = 6;
  must(!ClassicHfsRelinkSuccessor(&next, 9, &previous, 5, 8) && next.ndBLink == 6,
       "mismatched successor backlink remains intact");
  must(!ClassicHfsRelinkSuccessor(&next, 8, &previous, 6, 8) &&
       ClassicHfsRelinkSuccessor(&next, 9, &previous, 6, 8) && next.ndBLink == 8,
       "reject successor self-link and install valid replacement");
 }
 {
  unsigned char bytes[512] = {0}, map = 0x80;
  uint16_t offsets[36] = {14}; ClassicHfsNodeDescriptor descriptor = {0};
  int calls = 0;
  must(ClassicHfsTransferNode(8, 16, &map, 1, &bytes, &descriptor, offsets, 1,
       count_node_transfers, &calls) == ClassicHfsNodeUnavailable && calls == 0,
       "short bitmap blocks writes before storage access");
  must(ClassicHfsTransferNode(0, 0, NULL, 0, &bytes, &descriptor, offsets, 1,
       count_node_transfers, &calls) == ClassicHfsNodeUnavailable && calls == 0,
       "unmapped header cannot be written");
  ClassicHfsWrite16(bytes + 510, 14);
  must(ClassicHfsTransferNode(0, 0, NULL, 0, &bytes, &descriptor, offsets, 0,
       count_node_transfers, &calls) == ClassicHfsNodeOk && calls == 1,
       "read header before installing bitmap");
  bytes[19] = 42;
  descriptor.ndFLink = 17;
  int failure = -1;
  for (int writing = 0; writing <= 1; ++writing)
    must(ClassicHfsTransferNode(0, 1, &map, 1, &bytes, &descriptor, offsets, writing,
         partial_node_transfer, &failure) == ClassicHfsNodeTransferFailed &&
         bytes[19] == 42 && !bytes[3] && descriptor.ndFLink == 17 && offsets[0] == 14,
         "failed node transfer preserves caller data, descriptor and offsets");
  failure = 0;
  must(ClassicHfsTransferNode(0, 1, &map, 1, &bytes, &descriptor, offsets, 0,
       partial_node_transfer, &failure) == ClassicHfsNodeBadOffsets &&
       bytes[19] == 42 && offsets[0] == 14 && descriptor.ndFLink == 17,
       "malformed read preserves the prior decoded node");
 }
 {
  ClassicHfsExtent initial[3] = {{10,1},{12,1},{14,1}}, cached[3];
  unsigned start = 0, calls = 0, physical = 0, sector = 0;
  memcpy(cached, initial, sizeof(initial));
  must(ClassicHfsResolveForkSector(9, 2, 32, initial, cached, &start, load_test_fork_record,
       &calls, &physical, &sector) == 1 && physical == 22 && sector == 1 && start == 3 &&
       calls == 1, "map through overflow to sector within allocation block");
  must(ClassicHfsResolveForkSector(12, 2, 32, initial, cached, &start, load_test_fork_record,
       &calls, &physical, &sector) == 0 && start == 3 && physical == 22 && sector == 1,
       "failed overflow lookup leaves address unpublished");
  must(ClassicHfsResolveForkSector(0, 2, 32, initial, cached, &start, load_test_fork_record,
       &calls, &physical, &sector) == 1 && physical == 10 && start == 0 && calls == 2,
       "backward seek restores inline extent cache");
 }
 {
  unsigned char leaf[512] = {0}, key[8];
  uint16_t offsets[] = {14,34,54}; int slot = -1;
  ClassicHfsWriteExtentKey(leaf + 14, 0, 3, 3);
  ClassicHfsWriteExtentKey(leaf + 34, 0, 3, 9);
  ClassicHfsWriteExtentKey(key, 0, 3, 6);
  must(ClassicHfsScanExtentLeaf(leaf, offsets, 2, key, &slot) == ClassicHfsExtentMissing &&
       slot == -1, "extent bootstrap stops in sorted gap");
  must(!ClassicHfsExtentPayload(leaf, offsets, 2, 1, key), "overflow slot must match the requested key");
  ClassicHfsWriteExtentKey(key, 0, 3, 9);
  must(ClassicHfsExtentPayload(leaf, offsets, 2, 1, key) == 42, "validated overflow payload offset");
  offsets[2] = 514;
  must(!ClassicHfsExtentPayload(leaf, offsets, 2, 1, key) &&
       ClassicHfsScanExtentLeaf(leaf, offsets, 2, key, &slot) == ClassicHfsBadExtentLeaf,
       "reject overflow offset beyond the sector");
  offsets[2] = 52;
  must(ClassicHfsScanExtentLeaf(leaf, offsets, 2, key, &slot) == ClassicHfsBadExtentLeaf,
       "extent bootstrap rejects truncated extent payload");
 }
 {
  unsigned char bytes[512]; hfv_offset_t offset;
  must(ClassicHfsSectorOffset(UINT32_MAX, 2, 1, 1, &offset) &&
       offset == ((hfv_offset_t)UINT32_MAX + 1) * 512, "wide partition sector offset");
  must(!ClassicHfsSectorOffset(0, 2, 2, 1, &offset) &&
       !ClassicHfsSectorOffset(0, 0, 0, 1, &offset) &&
       ClassicHfsSectorOffset(0, 0, 2, 0, &offset), "reject unbounded writes, allow probing reads");
  must(ClassicHfsTransferSector(1, offset, bytes, 1, test_sector_seek, NULL, NULL) < 0 &&
       ClassicHfsTransferSector(0, offset, bytes, 1, test_sector_seek, NULL, short_sector_write) < 0,
       "reject displaced seek before transfer and reject a short write");
 }
 {
  ClassicHfsCacheEntry entries[ClassicHfsCacheSlots] = {0};
  CacheWriteLog log = {0};
  entries[0].bnum = UINT32_MAX; entries[1].bnum = 3; entries[2].bnum = 1;
  entries[0].flags = entries[1].flags = entries[2].flags = ClassicHfsCacheValid | ClassicHfsCacheDirty;
  must(ClassicHfsFlushCache(entries, 1, record_cache_write, &log) < 0 &&
       log.count == 3 && log.seen[0] == 1 && log.seen[1] == 3 && log.seen[2] == UINT32_MAX &&
       entries[0].bnum == UINT32_MAX && entries[0].flags == ClassicHfsCacheValid &&
       (entries[1].flags & ClassicHfsCacheDirty) && entries[2].flags == ClassicHfsCacheValid,
       "ordered flush preserves slots and retains failed sector for retry");
 }
 {
  ClassicHfsCache cache;
  ClassicHfsCacheEntry *entry;
  CacheWriteLog log = {0};
  ClassicHfsInitCache(&cache);
  for (unsigned i = 0; i < ClassicHfsCacheSlots; ++i) {
   must(ClassicHfsFindCacheEntry(&cache, i, &entry, record_cache_write, &log) == 0,
        "fill each cache slot once");
   entry->flags = ClassicHfsCacheValid | (i == 3 ? ClassicHfsCacheDirty : 0);
   (*entry->data)[0] = (unsigned char)i;
  }
  for (unsigned i = 0; i < 3; ++i)
   must(ClassicHfsFindCacheEntry(&cache, i, &entry, record_cache_write, &log) == 1,
        "promote cached sectors");
  must(ClassicHfsFindCacheEntry(&cache, 131, &entry, record_cache_write, &log) < 0 &&
       log.count == 1 && log.seen[0] == 3 && cache.tail->bnum == 3,
       "failed eviction keeps original hash and replacement order");
  must(ClassicHfsFindCacheEntry(&cache, 3, &entry, record_cache_write, &log) == 1 &&
       (*entry->data)[0] == 3 && (entry->flags & ClassicHfsCacheDirty),
       "dirty bytes remain readable after eviction failure");
  must(ClassicHfsFindCacheEntry(&cache, 131, &entry, record_cache_write, &log) == 0 &&
       entry->bnum == 131 && !entry->flags, "reserve clean victim for retry");
 }
 {
  ClassicHfsExtent tail[3] = {{2,2},{7,1},{12,1}}, added = {13,2}, out[3] = {0};
  must(ClassicHfsAppendExtent(tail, 8, 12, 32, added, out) == ClassicHfsExtendedRecord &&
       out[2].xdrNumABlks == 3 && tail[2].xdrNumABlks == 1, "stage contiguous growth");
  added.xdrStABN = 21;
  must(ClassicHfsAppendExtent(tail, 8, 12, 32, added, out) == ClassicHfsNewOverflowRecord &&
       out[0].xdrStABN == 21 && !out[1].xdrNumABlks, "start next overflow extent record");
  added.xdrStABN = 7;
  must(ClassicHfsAppendExtent(tail, 8, 12, 32, added, out) == ClassicHfsCannotAppend &&
       out[0].xdrStABN == 21, "reject allocation overlapping tail before publication");
 }
 {
  ClassicHfsExtent record[3] = {{20,4},{7,3},{2,2}};
  ClassicHfsTruncation plan;
  must(ClassicHfsPlanTruncation(record, 5, 10, 14, 32, &plan) &&
       plan.next == 14 && plan.kept[0].xdrNumABlks == 4 && plan.kept[1].xdrNumABlks == 1 &&
       plan.released[1].xdrStABN == 8 && plan.released[1].xdrNumABlks == 2 &&
       plan.released[2].xdrStABN == 2, "truncate within fragmented overflow extent");
  record[2].xdrStABN = 9;
  must(!ClassicHfsPlanTruncation(record, 5, 10, 14, 32, &plan) &&
       plan.released[2].xdrStABN == 2, "reject overlap before publishing releases");
 }
 {
  unsigned char out[520]; unsigned long pos = 510; uint32_t fail = 1;
  memset(out, 0xaa, sizeof(out));
  must(ClassicHfsReadFork(&fail, read_pattern_block, 1025, &pos, out, sizeof(out)) == -1 &&
       pos == 512 && out[0] == 254 && out[1] == 255 && out[2] == 0xaa,
       "fork read error preserves completed sector position");
  fail = UINT32_MAX;
  must(ClassicHfsReadFork(&fail, read_pattern_block, 1025, &pos, out, sizeof(out)) == 513 &&
       pos == 1025 && out[0] == 1 && out[511] == 0 && out[512] == 2 && out[513] == 0xaa &&
       ClassicHfsReadFork(&fail, read_pattern_block, 1025, &pos, NULL, UINT32_MAX) == 0,
       "retry full sector and partial tail up to logical EOF");
 }
 {
  ForkWriteTest disk;
  unsigned char input[] = {11,22,33,44};
  unsigned long pos = 507, logical = 512;
  memset(disk.bytes, 99, 512); disk.capacity = 512;
  must(ClassicHfsWriteFork(&disk,test_load_fork,test_store_fork,test_grow_fork,
       &disk.capacity,&logical,&pos,input,3) == 3 && disk.bytes[506] == 99 &&
       disk.bytes[507] == 11 && disk.bytes[509] == 33 && disk.bytes[510] == 99,
       "partial fork overwrite preserves both neighbors");
  must(ClassicHfsWriteFork(&disk,test_load_fork,test_store_fork,test_grow_fork,
       &disk.capacity,&logical,&pos,input,4) < 0 && disk.capacity == 1024 &&
       pos == 512 && logical == 512 && disk.bytes[510] == 11 && disk.bytes[511] == 22,
       "failed second write preserves completed bytes and logical EOF");
 }
 {
  uint16_t blocks = 17;
  uint32_t bytes = 99;
  must(ClassicHfsGrowthRequest(1,512,512,&blocks) == ClassicHfsBadForkSize &&
       ClassicHfsGrowthRequest(0,0,512,&blocks) == ClassicHfsBadForkSize && blocks == 17,
       "reject invalid physical fork before allocation");
  must(ClassicHfsGrowthRequest(0xfffffc00,512,4096,&blocks) == ClassicHfsForkSizeOk &&
       blocks == 1 && ClassicHfsGrowthRequest(0xfffffe00,512,0,&blocks) == ClassicHfsForkSizeLimit,
       "limit clump to last representable allocation block");
  must(ClassicHfsTruncatedSize(UINT32_MAX,0xfffffe00,512,&bytes) == ClassicHfsForkSizeLimit &&
       bytes == 99 && ClassicHfsTruncatedSize(513,1536,512,&bytes) == ClassicHfsForkSizeOk &&
       bytes == 1024, "round retained physical EOF without 32-bit wraparound");
 }
 {
  unsigned long position = 3;
  must(ClassicHfsSeekFork(UINT32_MAX, &position, INT64_MAX, SEEK_CUR) &&
       position == UINT32_MAX && ClassicHfsSeekFork(UINT32_MAX, &position, -1, SEEK_END) &&
       position == UINT32_MAX - 1, "relative fork seek clamps before signed overflow");
  must(!ClassicHfsSeekFork(UINT32_MAX, &position, 0, -1) && position == UINT32_MAX - 1 &&
       ClassicHfsSeekFork(UINT32_MAX, &position, INT64_MIN, SEEK_CUR) && !position,
       "invalid origin preserves position and minimum displacement clamps to start");
  must(ClassicHfsSeekFork(10, &position, 4, SEEK_SET) && position == 4 &&
       ClassicHfsSeekFork(10, &position, -3, SEEK_CUR) && position == 1,
       "absolute and relative fork seeks preserve interior offsets");
 }
 {
  char name[32]; const char *path = "disk:Folder::Other:";
  must(ClassicHfsBeginPath(&path,"Disk",0)==ClassicHfsPathAbsolute &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathName && !strcmp(name,"disk") &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathName && !strcmp(name,"Folder") &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathParent &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathName && !strcmp(name,"Other") &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathEnd, "absolute path and parent navigation");
  path="Other::File";
  must(ClassicHfsBeginPath(&path,"Disk",0)==ClassicHfsPathWrongVolume &&
       ClassicHfsBeginPath(&path,"Disk",1)==ClassicHfsPathRelative &&
       ClassicHfsNextPathPart(&path,name)==ClassicHfsPathParent, "legacy CD label tolerance");
 }
 {
  unsigned char record[160] = {0};
  int keyBytes = ClassicHfsWriteCatalogKey(record, 2, "StoredCase");
  ClassicHfsCatalogKey key;
  const int sizes[] = {70, 102, 46, 46};
  for (unsigned type = 1; type <= 4; ++type) {
   record[keyBytes] = (unsigned char)type;
   must(!ClassicHfsCatalogPayload(record, keyBytes + sizes[type-1] - 1, NULL) &&
        ClassicHfsCatalogPayload(record, keyBytes + sizes[type-1], &key) == keyBytes &&
        !strcmp(key.ckrCName,"StoredCase"), "catalog payload bounds and stored spelling");
  }
  record[keyBytes] = 9;
  must(!ClassicHfsCatalogPayload(record, sizeof(record), NULL), "reject unknown catalog type");
 }
 {
  unsigned char body[45], encoded[45];
  ClassicHfsCatalogThread thread;
  for (unsigned i=0;i<sizeof(body);++i) body[i]=(unsigned char)(0x80+i);
  body[12]=31;
  ClassicHfsDecodeThread(body,&thread);
  memset(encoded,0,sizeof(encoded));
  ClassicHfsEncodeThread(encoded,&thread);
  must(thread.thdParID==UINT32_C(0x88898a8b) && thread.thdCName[31]==0 &&
       !memcmp(body,encoded,44) && encoded[44]==0, "thread reserved fields, parent and full MacRoman name");
 }
 {
  unsigned char input[68], output[69] = {0};
  ClassicHfsDirectory dir;
  for (unsigned i=0;i<68;++i) input[i]=(unsigned char)(0x80+i);
  ClassicHfsDecodeDirectory(input,&dir);
  must(dir.dirDirID==UINT32_C(0x84858687) && (uint32_t)dir.dirBkDat==UINT32_C(0x90919293) &&
       (uint16_t)dir.dirUsrInfo.frRect.top==0x9495 &&
       (uint32_t)dir.dirFndrInfo.frOpenChain==UINT32_C(0xa8a9aaab),
       "directory ID, backup date, and Finder fields at wire offsets");
  ClassicHfsEncodeDirectory(output,&dir);
  must(!memcmp(input,output,68) && !output[68], "directory body round trip includes reserved fields");
 }
 {
  unsigned char input[100], output[101] = {0};
  ClassicHfsCatalogFile file;
  for (unsigned i=0;i<100;++i) input[i]=(unsigned char)(0x80+i);
  ClassicHfsDecodeFile(input,&file);
  must((uint32_t)file.filUsrWds.fdType==UINT32_C(0x82838485) &&
       file.filLgLen==UINT32_C(0x98999a9b) && file.filRLgLen==UINT32_C(0xa2a3a4a5) &&
       file.filExtRec[0].xdrStABN==0xc8c9 && file.filRExtRec[0].xdrStABN==0xd4d5,
       "file Finder type, fork lengths and inline extents at wire offsets");
  ClassicHfsEncodeFile(output,&file);
  must(!memcmp(input,output,100) && !output[100], "complete catalog file body round trip");
 }
 {
  ClassicHfsCatalogRecord record = {0};
  unsigned char input[102] = {0}, output[103];
  for (unsigned type=1;type<=4;++type) {
   unsigned size=ClassicHfsRecordSize(type);
   input[0]=(unsigned char)type; input[1]=0x91;
   record.cdrType=0;
   must(!ClassicHfsDecodeCatalogRecord(input,size-1,&record) && !record.cdrType &&
        ClassicHfsDecodeCatalogRecord(input,size,&record)==size, "bounded catalog decode");
   memset(output,0xcc,sizeof(output));
   must(!ClassicHfsEncodeCatalogRecord(output,size-1,&record) && output[0]==0xcc &&
        ClassicHfsEncodeCatalogRecord(output,size,&record)==size && !memcmp(input,output,size) &&
        output[size]==0xcc, "bounded catalog encode and reserved type byte");
  }
 }
 {
  ClassicHfsCatalogRecord record = {0}; hfsdirent entry = {0};
  record.cdrType = cdrFilRec;
  record.u.fil.filFlags = 0x42;
  record.u.fil.filFlNum = 100;
  record.u.fil.filRLgLen = 200;
  record.u.fil.filResrv = 300;
  ClassicHfsDescribeEntry(2,"Example",&record,&entry);
  must(entry.cnid==100 && entry.u.file.rsize==200 && !strcmp(entry.name,"Example"),
       "describe file identity and resource length");
  entry.flags |= HFS_ISLOCKED; entry.cnid=0; entry.u.file.rsize=0;
  memcpy(entry.u.file.type,"TEXT",4);
  ClassicHfsApplyEntry(&record,&entry);
  must(record.u.fil.filFlags==0x43 && record.u.fil.filUsrWds.fdType==0x54455854 &&
       record.u.fil.filFlNum==100 && record.u.fil.filRLgLen==200 && record.u.fil.filResrv==300,
       "entry edits preserve identity, fork length, reserved fields and other flags");
 }
 {
  unsigned char source[106], output[107];
  ClassicHfsTreeHeader header;
  size_t i;
  for (i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(0x80 + i);
  memset(output, 0x55, sizeof(output));
  must(!ClassicHfsDecodeTreeHeader(source, 105, &header) &&
       ClassicHfsDecodeTreeHeader(source, 106, &header) && header.bthDepth == 0x8081 &&
       header.bthFree == UINT32_C(0x9a9b9c9d), "tree header boundaries and field order");
  must(!ClassicHfsEncodeTreeHeader(output, 105, &header) && output[0] == 0x55 &&
       ClassicHfsEncodeTreeHeader(output, 106, &header) && !memcmp(output, source, 106) &&
       output[106] == 0x55, "preserve tree header reserved bytes and following record");
 }
 {
  unsigned char bytes[2] = {0};
  ClassicHfsSetBit(bytes, 0); ClassicHfsSetBit(bytes, 7); ClassicHfsSetBit(bytes, 8);
  ClassicHfsClearBit(bytes, 7);
  must(bytes[0] == 0x80 && bytes[1] == 0x80, "most significant allocation bit first");
  for (unsigned mask = 0; mask < 256; ++mask) {
   bytes[0] = (unsigned char)mask;
   for (unsigned hint = 0; hint <= 8; ++hint) for (unsigned request = 1; request <= 9; ++request) {
    unsigned best = 0, expected = 99, actual = 99;
    for (unsigned step = 0; step < 8; ++step) {
     unsigned start = (hint + step) % 8, run = 0;
     while (start + run < 8 && run < request && !(mask & (128u >> (start + run)))) ++run;
     if (run > best) { best = run; expected = start; }
    }
    must(ClassicHfsFindFreeRun(bytes, 8, hint, request, &actual) == best && actual == expected,
         "allocation hint, partial extent and wraparound");
   }
  }
 }
 {
  unsigned char map = 0xf0;
  uint16_t hint = 4, freeBlocks = 9, first = 99;
  must(!ClassicHfsAllocateBlocks(&map, 8, &hint, &freeBlocks, 4, &first) &&
       map == 0xf0 && hint == 4 && freeBlocks == 9 && first == 99,
       "impossible allocation count preserves bitmap, hint and extent");
  freeBlocks = 4;
  must(ClassicHfsAllocateBlocks(&map, 8, &hint, &freeBlocks, 6, &first) == 4 &&
       map == 0xff && hint == 0 && freeBlocks == 0 && first == 4,
       "partial allocation updates bitmap and wraps hint");
 }
 {
  unsigned char map[2] = {0x07, 0xe0};
  uint16_t freeBlocks = 10;
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 7, &freeBlocks) &&
       map[0] == 7 && map[1] == 0xe0 && freeBlocks == 10, "partial double free is atomic");
  must(!ClassicHfsReleaseBlocks(map, 16, 15, 2, &freeBlocks) &&
       !ClassicHfsReleaseBlocks(map, 16, UINT32_MAX, 1, &freeBlocks), "invalid release range");
  freeBlocks = 15;
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) &&
       map[0] == 7 && map[1] == 0xe0 && freeBlocks == 15, "invalid free count preserves bitmap");
  freeBlocks = 10;
  must(ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) &&
       map[0] == 0 && map[1] == 0 && freeBlocks == 16, "release across bitmap bytes");
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) && freeBlocks == 16,
       "reject repeated release");
 }
 {
  ClassicHfsExtent runs[3] = {{18, 2}, {4, 3}, {0, 1}};
  unsigned location = 99;
  must(ClassicHfsLocateBlock(runs, 20, 7, 8, &location) == ClassicHfsMappedBlock && location == 19,
       "last physical allocation block");
  must(ClassicHfsLocateBlock(runs, 20, 7, 9, &location) == ClassicHfsMappedBlock && location == 4 &&
       ClassicHfsLocateBlock(runs, 20, 7, 12, &location) == ClassicHfsMappedBlock && location == 0,
       "fragment boundaries");
  must(ClassicHfsLocateBlock(runs, 20, 7, 13, &location) == ClassicHfsNextExtentRecord && location == 13,
       "next overflow key");
  runs[0].xdrNumABlks = 3;
  must(ClassicHfsLocateBlock(runs, 20, 7, 7, &location) == ClassicHfsInvalidExtent && location == 13,
       "reject physically truncated run before I/O");
  runs[0].xdrNumABlks = 0;
  must(ClassicHfsLocateBlock(runs, 20, 7, 7, &location) == ClassicHfsInvalidExtent,
       "missing mapping cannot advance overflow search");
 }
 {
  uint32_t sector = 77;
  must(ClassicHfsAllocationSector(16, 65535, 256, 16776978, 65534, 255, &sector) &&
       sector == 16776975, "last allocation sector on a large image");
  must(!ClassicHfsAllocationSector(16, 65535, 256, sector, 65534, 255, &sector) &&
       !ClassicHfsAllocationSector(16, 65535, 256, UINT32_MAX, 1, 256, &sector) &&
       !ClassicHfsAllocationSector(16, 65535, 0x100000, UINT32_MAX, 4096, 0, &sector) &&
       sector == 16776975, "reject partition overrun, invalid index and wrapped sector");
  must(ClassicHfsVolumeSectors(16, 65535, 256, &sector) && sector == 16776978 &&
       !ClassicHfsVolumeSectors(16, 65535, 0x100000, &sector), "checked classic volume length");
 }
 must(argc==2&&strlen(argv[1])<900,"test directory argument");must(CreateDirectoryA(argv[1],NULL),"exclusive test directory");
 sprintf(image,"%s\\binhex5.dsk",argv[1]);sprintf(path,"%s\\roundtrip.bin",argv[1]);sprintf(badpath,"%s\\bad.bin",argv[1]);
 host=CreateFileA(image,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);must(host!=INVALID_HANDLE_VALUE,"create synthetic image");length.QuadPart=16*1024*1024;must(SetFilePointerEx(host,length,NULL,FILE_BEGIN)&&SetEndOfFile(host),"size image");CloseHandle(host);
 must(hfs_format(image,-1,label)<0 && errno==EINVAL,"negative partition cannot select a whole-image format");
 must(hfs_format(image,0,label)==0,"format image");verify_formatted_tree_headers(image);verify_repeated_tree_map(image);v=hfs_mount(image,0,O_RDWR);must(v!=NULL,"mount image");
 {
  uint32_t nextId = v->mdb.drNxtCNID;
  must(hfs_mkdir(v, ":MissingParent:Nested") < 0 && errno == ENOENT &&
       hfs_create(v, ":MissingParent:Nested", "TEXT", "TEST") < 0 && errno == ENOENT &&
       v->mdb.drNxtCNID == nextId, "missing creation parent preserves next catalog ID");
  must(hfs_create(v, ":InvalidCreator", "TEXT", NULL) < 0 && errno == EINVAL &&
       v->mdb.drNxtCNID == nextId, "invalid file creator rejected before mutation");
 }
 {
  CatDataRec entry, thread, saved;
  node slot;
  long previous = v->cwd;
  must(hfs_mkdir(v, ":PathSource") == 0 && hfs_mkdir(v, ":PathOther") == 0, "create directory thread fixtures");
  must(ClassicHfsResolvePath(v, ":PathSource", &entry, NULL, NULL, NULL) == 1, "resolve source directory");
  v->cwd = entry.u.dir.dirDirID;
  must(ClassicHfsFindCatalogThread(v, v->cwd, &thread, &slot, cdrThdRec) == 1, "read source thread");
  saved = thread;
  strcpy(thread.u.dthd.thdCName, "PathOther");
  must(ClassicHfsStoreCatalogEntry(&thread, &slot) == 0, "stage misdirected directory thread");
  must(ClassicHfsResolvePath(v, ":", &entry, NULL, NULL, NULL) < 0 && errno == EIO &&
       ClassicHfsResolvePath(v, "::PathOther", &entry, NULL, NULL, NULL) < 0 && errno == EIO,
       "reject misdirected thread for current and parent-relative paths");
  long savedId = v->cwd;
  char unchanged[32] = "original";
  must(hfs_dirinfo(v, &savedId, unchanged) < 0 && errno == EIO && savedId == v->cwd &&
       !strcmp(unchanged, "original"), "directory info preserves outputs for misdirected thread");
  unsigned long oldFiles = v->mdb.drFilCnt;
  must(ClassicHfsAdjustEntryCount(v, v->cwd, 0, 1) < 0 && errno == EIO &&
       v->mdb.drFilCnt == oldFiles, "misdirected thread cannot change parent or volume count");
  must(ClassicHfsStoreCatalogEntry(&saved, &slot) == 0 &&
       ClassicHfsResolvePath(v, ":", &entry, NULL, NULL, NULL) == 1 && entry.u.dir.dirDirID == v->cwd,
       "restored thread resolves the original directory");
  v->cwd = previous;
  must(hfs_rmdir(v, ":PathSource") == 0 && hfs_rmdir(v, ":PathOther") == 0, "remove thread fixtures");
 }
 {
  {
   hfsfile bounded = v->cat.f;
   block sector = {0};
   unsigned long length = bounded.cat.u.fil.filPyLen;
   bounded.cat.u.fil.filPyLen = 0;
   must(length >= 512 && ClassicHfsReadForkSector(&bounded, 0, &sector) < 0 &&
        ClassicHfsWriteForkSector(&bounded, 0, &sector) < 0 &&
        hfs_error && strstr(hfs_error, "outside classic HFS physical fork"),
        "reject sector zero of empty fork despite mapped catalog extents");
  }
  hfsfile damaged = {0};
  damaged.vol = v;
  damaged.cat.u.fil.filExtRec[0].xdrNumABlks = 1;
  unsigned freeBefore = v->mdb.drFreeBks, firstBefore = v->mdb.drAllocPtr;
  must(ClassicHfsExtendForkAllocation(&damaged) < 0 && v->mdb.drFreeBks == freeBefore &&
       v->mdb.drAllocPtr == firstBefore && damaged.cat.u.fil.filPyLen == 0,
       "reject extents on empty fork before claiming allocation blocks");
  btree *tree = &v->cat;
  node before = tree->hdrnd;
  int flags = tree->flags;
  tree->flags |= HFS_UPDATE_BTHDR;
  tree->hdrnd.nd.ndFLink = 1;
  must(tree->mapsz == HFS_MAP1SZ && ClassicHfsFlushTreeHeader(tree) < 0 &&
       (tree->flags & HFS_UPDATE_BTHDR) && !memcmp(before.data, tree->hdrnd.data, sizeof(before.data)),
       "reject unexpected map continuation before publishing header");
  uint32_t nodes = tree->hdr.bthNNodes;
  tree->hdr.bthNNodes = HFS_MAP1SZ * 8 + 1;
  char *originalMap = tree->map;
  must(ClassicHfsGrowTreeMap(tree) < 0 && tree->map == originalMap && tree->mapsz == HFS_MAP1SZ,
       "reject unexpected map tail before reallocating or linking a new node");
  tree->hdr.bthNNodes = nodes;
  tree->hdrnd = before;
  tree->flags = flags;
  unsigned char truncatedKey[2] = {7, 0};
  ClassicHfsTreeHeader emptyHeader = v->ext.hdr;
  must(!emptyHeader.bthRoot && ClassicHfsInsertTreeRecord(&v->ext, truncatedKey, sizeof(truncatedKey)) < 0 &&
       !memcmp(&emptyHeader, &v->ext.hdr, sizeof(emptyHeader)),
       "truncated insertion leaves empty extents tree unallocated");
  unsigned char absent[40];
  ClassicHfsWriteCatalogKey(absent, HFS_CNID_ROOTDIR, "missing deletion target");
  ClassicHfsTreeHeader savedHeader = tree->hdr;
  must(ClassicHfsDeleteTreeKey(tree, absent) < 0 &&
       !memcmp(&savedHeader, &tree->hdr, sizeof(savedHeader)) && flags == tree->flags,
       "unsuccessful delete descent leaves tree accounting unchanged");
  uint32_t originalNodes = tree->hdr.bthNNodes;
  must(ClassicHfsReserveTreeSpace(tree, UINT32_MAX) < 0 && tree->hdr.bthNNodes == originalNodes,
       "reject impossible tree reservation before growth");
  unsigned records = 1200;
  must(ClassicHfsReserveTreeSpace(tree, records) == 0 &&
       tree->hdr.bthFree >= records * (tree->hdr.bthDepth + 1u) &&
       tree->mapsz > HFS_MAP1SZ, "multi-clump reservation accounts for new map nodes");
 }
 must(ClassicHfsCompareNames("A", "a") == 0 && ClassicHfsCompareNames("a", "ab") < 0 &&
      ClassicHfsCompareNames("\x80", "\x8a") == 0 && ClassicHfsCompareNames("a", "\x80") < 0 &&
      ClassicHfsCompareNames(" ", "\xca") == 0, "classic MacRoman name ordering");
 must(hfs_create(v,"BH5:\x80","TEXT","TEST")==0,"create accented catalog name");
 must(hfs_create(v,"bh5:\x8a","TEXT","TEST")<0,"reject equivalent catalog name");
 for(k=0;k<sizeof(cases)/sizeof(cases[0]);k++){
  hfsfile*f;hfsdirent original,restored;unsigned char bytes[5000],*exported;unsigned size,i;unsigned d=cases[k][0],r=cases[k][1];
  if(k)must(hfs_delete(v,source)==0,"remove previous source");
  must(hfs_create(v,source,"APPL","TEST")==0,"create source");f=hfs_open(v,source);must(f!=NULL,"open source");
  for(fork=0;fork<2;fork++){unsigned n=fork?r:d;for(i=0;i<n;i++)bytes[i]=value(i,fork);must(hfs_setfork(f,fork)==0&&hfs_write(f,bytes,n)==n,"write source fork");}
  must(hfs_fstat(f,&original)==0,"source metadata");original.fdflags=HFS_FNDR_ISINVISIBLE|HFS_FNDR_HASBUNDLE|HFS_FNDR_COLOR;original.fdlocation.v=-11;original.fdlocation.h=37;original.crdate=1600000000;original.mddate=1600000010;must(hfs_fsetattr(f,&original)==0&&hfs_close(f)==0,"set source metadata");
  must(cpo_macb1(v,source,path)==0,"BinHex 5 export");exported=load(path,&size);
  must(size==128+((d+127)&~127u)+((r+127)&~127u),"MacBinary I aligned file length");
  must(exported[0]==0&&exported[74]==0&&exported[82]==0,"legacy header zero bytes");for(i=99;i<128;i++)must(exported[i]==0,"MacBinary I reserved tail zero");
  must(!memcmp(exported+65,"APPLTEST",8)&&exported[73]==(unsigned char)(original.fdflags>>8),"type creator high Finder flags");
  must((unsigned long)ClassicHfsRead32(exported+83)==d&&(unsigned long)ClassicHfsRead32(exported+87)==r,"unsigned fork lengths");
  must(cpi_macb(path,v,target)==0,"MacBinary I import");verify(v,target,d,r);must(hfs_stat(v,target,&restored)==0,"imported metadata");
  must(restored.fdflags==(original.fdflags&0xff00)&&restored.fdlocation.v==-11&&restored.fdlocation.h==37&&restored.crdate==original.crdate&&restored.mddate==original.mddate,"legacy metadata and low-flag limitation");
  // A final fork need not carry transport padding. Inter-fork padding remains.
  if(d||r){unsigned minimum=128+(r?((d+127)&~127u)+r:d);save(badpath,exported,minimum);must(cpi_macb(badpath,v,target)==0,"unpadded final fork import");verify(v,target,d,r);}
  // Structural errors must be rejected before overwriting an existing target.
  if(d||r){unsigned minimum=128+(r?((d+127)&~127u)+r:d);save(badpath,exported,minimum-1);must(cpi_macb(badpath,v,target)==-1&&strstr(cpi_error,"truncated"),"truncated source preflight");verify(v,target,d,r);}
  exported[110]=1;save(badpath,exported,size);must(cpi_macb(badpath,v,target)==-1,"invalid legacy reserved tail");verify(v,target,d,r);free(exported);
  must(cpo_macb(v,source,path)==0,"existing MacBinary II export");exported=load(path,&size);
  must(exported[122]==129&&exported[123]==129&&(unsigned short)ClassicHfsRead16(exported+124)==crc_macb(exported,124,0),"MacBinary II version and CRC unchanged");
  must(cpi_macb(path,v,target)==0,"MacBinary II import");verify(v,target,d,r);
  exported[65]^=1;save(badpath,exported,size);must(cpi_macb(badpath,v,target)==-1&&strstr(cpi_error,"CRC"),"corrupt II must not downgrade to I");verify(v,target,d,r);free(exported);
 }
 {
  unsigned char* b;unsigned size;hfsdirent e;unsigned i;
  must(cpo_macb1(v,source,path)==0,"prepare legacy header edge tests");b=load(path,&size);
  b[1]=63;memset(b+2,'A',63);save(badpath,b,size);must(cpi_macb(badpath,v,target)==0,"Pascal 63-byte name without terminator");verify(v,target,4097,3073);
  b[1]=0;save(badpath,b,size);must(cpi_macb(badpath,v,target)==-1,"empty header name rejected");
  b[1]=4;memcpy(b+2,"X:Y!",4);save(badpath,b,size);must(cpi_macb(badpath,v,target)==-1,"colon in name rejected");
  b[1]=4;memcpy(b+2,"Name",4);b[81]=1;save(badpath,b,size);must(cpi_macb(badpath,v,target)==0&&hfs_stat(v,target,&e)==0&&(e.flags&HFS_ISLOCKED),"protected flag restored");
  must(cpo_macb1(v,target,path)==0,"protected file export");free(b);b=load(path,&size);must(b[81]==1,"protected flag exported");free(b);
  // Versioned files may have a secondary header; its rounded size is honored.
  must(cpo_macb(v,source,path)==0,"secondary header fixture");b=load(path,&size);b=realloc(b,size+128);must(b!=NULL,"secondary header allocation");memmove(b+256,b+128,size-128);memset(b+128,0x5a,128);ClassicHfsWrite16(b+120,3);ClassicHfsWrite16(b+124,crc_macb(b,124,0));save(badpath,b,size+128);
  {char other[]="BH5:Secondary";must(cpi_macb(badpath,v,other)==0,"MacBinary II secondary header skip");verify(v,other,4097,3073);}
  // No silent signed-length overflow on either legacy fork.
  for(i=0;i<2;i++){memset(b,0,128);b[1]=1;b[2]='X';b[83+4*i]=0x80;save(badpath,b,128);{char absent[]="BH5:TooLarge";must(cpi_macb(badpath,v,absent)==-1&&strstr(cpi_error,"bad file length")&&hfs_stat(v,absent,&e)==-1,"legacy fork limit preflight");}}
  free(b);
 }
 for(int i=79;i>=0;--i){char name[32];sprintf(name,"BH5:Descending%03d",i);
  must(hfs_create(v,name,"TEXT","TEST")==0,"descending catalog split insertion");}
 must(hfs_umount(v)==0,"persist image");v=hfs_mount(image,0,O_RDONLY);must(v!=NULL,"reopen image");verify(v,target,4097,3073);verify(v,"bh5:\x8a",0,0);
 for(int i=0;i<80;++i){char name[32];hfsdirent entry;sprintf(name,"BH5:Descending%03d",i);
  must(hfs_stat(v,name,&entry)==0,"reopen and locate every split catalog record");}
 must(hfs_umount(v)==0,"close image");
 v=hfs_mount(image,0,O_RDWR);must(v!=NULL,"reopen for catalog merges");
 for(int i=10;i<70;++i){char name[32];sprintf(name,"BH5:Descending%03d",i);
  must(hfs_delete(v,name)==0,"delete middle range across catalog siblings");}
 must(hfs_umount(v)==0,"persist catalog merges");
 v=hfs_mount(image,0,O_RDONLY);must(v!=NULL,"reopen merged catalog");
 for(int i=0;i<80;++i){char name[32];hfsdirent entry;sprintf(name,"BH5:Descending%03d",i);
  must((hfs_stat(v,name,&entry)==0)==(i<10 || i>=70),"catalog survivors after sibling merges");}
 verify(v,target,4097,3073);must(hfs_umount(v)==0,"close merged catalog");
 puts("PASS: BinHex 5/MacBinary I import/export, empty and aligned/unaligned forks, metadata/protected flag, Pascal names, unpadded final fork, preflight preservation, II CRC downgrade protection, secondary headers and Apple fork limits; synthetic files only");return 0;
}
