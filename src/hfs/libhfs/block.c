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

# include <sys/types.h>

#ifdef WIN32
# include <io.h>
# include <windows.h>
# undef ERROR
# include <stdio.h>
#else
# include <unistd.h>
#endif

# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <time.h>

# ifdef DEBUG
# include <stdio.h>
# endif

# include "internal.h"
#ifdef WIN32
# include "../../ClassicHfsRecovery.h"
#endif
#include "../ClassicHfsTime.h"
#include "../ClassicHfsGeometry.h"
# include "block.h"
# include "low.h"

#include "../ClassicHfsSectorIO.h"

static HFV_THREAD_LOCAL int rollback_status;
void hfs_note_rollback(int status) {
 if(status>rollback_status)rollback_status=status;
}
int hfs_take_rollback(void) {
 int status=rollback_status;
 rollback_status=0;
 return status;
}

/*
 * NAME:	block->init()
 * DESCRIPTION:	initialize a volume's block cache
 */
int b_init(hfsvol *vol)
{
  bcache *cache;

# ifdef DEBUG
  if (vol->cache)
    abort();
# endif

  cache = ALLOC(bcache, 1);
  if (cache == 0)
    {
      ERROR(ENOMEM, 0);
      return -1;
    }

  vol->cache = cache;
  ClassicHfsInitCache(cache);
  return 0;
}

# ifdef DEBUG
/*
 * NAME:	b_debugcache()
 * DESCRIPTION:	dump the cache tables for a volume
 */
void b_debugcache(hfsvol *vol)
{
  bcache *cache = vol->cache;
  int i;
  bucket *b;

  printf("CACHE DUMP:\n");

  for (i = 0, b = cache->tail->cnext; i < HFS_CACHESZ; ++i, b = b->cnext)
    {
      if (b->flags & HFS_BLOCK_INUSE)
	{
	  printf("\t %lu", b->bnum);
	  if (b->flags & HFS_BLOCK_DIRTY)
	    printf("*");

	}
    }

  printf("\n");

  printf("HASH DUMP:\n");

  for (i = 0; i < ClassicHfsCacheHashSize; ++i)
    {
      int seen = 0;

      b = &cache->hash[i];

      while ((b = b->hnext))
	{
	  if (! seen)
	    printf("  %d:", i);

	  if (b->flags & HFS_BLOCK_INUSE)
	    {
	      printf(" %lu", b->bnum);
	      if (b->flags & HFS_BLOCK_DIRTY)
		printf("*");

	    }

	  seen = 1;
	}

      if (seen)
	printf("\n");
    }
}
# endif

/*
 * NAME:	getblock()
 * DESCRIPTION:	read a block from the physical device
 */
static
int getblock(hfsvol *vol, unsigned long bnum, block *bp)
{
  hfv_offset_t offset;
  if (!ClassicHfsSectorOffset(vol->vstart, vol->vlen, bnum, 0, &offset)) {
    ERROR(EIO, "classic HFS read outside volume");
    return -1;
  }
  if (ClassicHfsTransferSector(vol->fd, offset, bp, 0, lseek, read, write) < 0) {
    ERROR(errno, "cannot read complete classic HFS sector");
    return -1;
  }
  return 0;
}

/* Runtime undo for serialized classic-HFS metadata operations. This is not
 * a persistent journal: it handles reported errors, not process/power loss. */
#define UNDO_BUCKETS 2048
#define UNDO_MAX_BLOCKS 65536
struct undo_block { unsigned long sector; unsigned next; block before; };
struct file_view { hfsfile *file; hfsfile saved; };
struct mutation {
 hfsvol *volume;
 hfsvol saved;
 block *bitmap;
 char *extmap,*catmap;
 bcache *cache;
 struct undo_block *blocks;
 struct file_view *files;
 unsigned file_count,used,capacity,buckets[UNDO_BUCKETS];
};
#define active_mutation (hfs_context_get()->mutation)
static int putblock(hfsvol *, unsigned long, block *);
static void free_mutation(struct mutation *m) {
 FREE(m->bitmap);FREE(m->extmap);FREE(m->catmap);FREE(m->cache);FREE(m->files);FREE(m->blocks);FREE(m);
}
static void *save_memory(const void *p,size_t size) {
 void *copy;if(!p||!size)return 0;
 copy=malloc(size);if(copy)memcpy(copy,p,size);return copy;
}
int b_begin_mutation(hfsvol *vol) {
 struct mutation *m;size_t bitmap_size;hfsfile *file;unsigned i;
 if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes disabled after rollback failure");return -1;}
 if(vol->flags&HFS_READONLY){ERROR(EROFS,"Volume is read-only");return -1;}
 if(active_mutation){ERROR(EBUSY,"Nested classic HFS mutation");return -1;}
 m=(struct mutation*)calloc(1,sizeof(*m));
 if(!m){ERROR(ENOMEM,"Cannot allocate HFS undo state");return -1;}
 m->volume=vol;m->saved=*vol;
 bitmap_size=SIZE(block,ClassicHfsBitmapSectors(vol->mdb.drNmAlBlks));
 m->bitmap=(block*)save_memory(vol->vbm,bitmap_size);
 m->extmap=(char*)save_memory(vol->ext.map,vol->ext.mapsz);
 m->catmap=(char*)save_memory(vol->cat.map,vol->cat.mapsz);
 m->cache=(bcache*)save_memory(vol->cache,sizeof(bcache));
 if((vol->vbm&&!m->bitmap)||(vol->ext.map&&!m->extmap)||(vol->cat.map&&!m->catmap)||(vol->cache&&!m->cache)) {
  free_mutation(m);ERROR(ENOMEM,"Cannot snapshot HFS metadata");return -1;
 }
 for(file=vol->files;file;file=file->next) {
  if(++m->file_count>65536){free_mutation(m);ERROR(ENOMEM,"Too many open HFS file views");return -1;}
 }
 if(m->file_count) {
  m->files=ALLOC(struct file_view,m->file_count);
  if(!m->files){free_mutation(m);ERROR(ENOMEM,"Cannot snapshot open HFS file views");return -1;}
  for(file=vol->files,i=0;file;file=file->next,++i){m->files[i].file=file;m->files[i].saved=*file;}
 }
 active_mutation=m;
 /* Earlier pending writes also need undo coverage before they reach disk. */
 if(hfs_flush_raw(vol)<0){b_end_mutation(vol,-1);return -1;}
 return 0;
}
static int remember_block(hfsvol *vol,unsigned long sector) {
 struct mutation *m=active_mutation;unsigned bucket,index;
 if(!m||m->volume!=vol)return 0;
 bucket=(sector^(sector>>11))&(UNDO_BUCKETS-1);
 for(index=m->buckets[bucket];index;index=m->blocks[index-1].next)
  if(m->blocks[index-1].sector==sector)return 0;
 if(m->used==UNDO_MAX_BLOCKS){ERROR(ENOMEM,"HFS metadata undo limit exceeded");return -1;}
 if(m->used==m->capacity) {
  unsigned capacity=m->capacity?m->capacity*2:128;
  struct undo_block *blocks=(struct undo_block*)realloc(m->blocks,SIZE(struct undo_block,capacity));
  if(!blocks){ERROR(ENOMEM,"Cannot grow HFS undo records");return -1;}
  m->blocks=blocks;m->capacity=capacity;
 }
 if(getblock(vol,sector,&m->blocks[m->used].before)<0)return -1;
 m->blocks[m->used].sector=sector;m->blocks[m->used].next=m->buckets[bucket];
 m->buckets[bucket]=++m->used;return 0;
}
/* Failed rollback evidence, not a write-ahead/crash journal. The 512-byte
 * HFSUNDO1 header records source identity and volume range. Each 528-byte
 * record is LE absolute byte offset(8), CRC32(4), length(4), original bytes(512).
 * No automatic replay: source identity/media must be checked before recovery. */
#ifdef WIN32
static void undo_u32(unsigned char *p,unsigned long n) {unsigned i;for(i=0;i<4;++i){p[i]=(unsigned char)n;n>>=8;}}
static void undo_u64(unsigned char *p,hfv_size_t n) {unsigned i;for(i=0;i<8;++i){p[i]=(unsigned char)n;n>>=8;}}
static unsigned long undo_crc(const unsigned char *p,size_t n) {
 unsigned long crc=0xffffffffUL;unsigned bit;
 while(n--){crc^=*p++;for(bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320UL:0);}
 return ~crc;
}
static int save_failed_undo(struct mutation *m,char *path) {
 char directory[MAX_PATH];unsigned char header[512]={0},record[528];HANDLE file;DWORD written;unsigned i;int ok=1;
 DWORD n=GetTempPathA(sizeof(directory),directory);
 if(!n||n>=sizeof(directory)||!GetTempFileNameA(directory,"hfu",0,path))return 0;
 file=CreateFileA(path,GENERIC_WRITE|DELETE,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
 if(file==INVALID_HANDLE_VALUE)return 0;
 memcpy(header,"HFSUNDO1",8);undo_u32(header+8,1);undo_u32(header+12,512);
 undo_u64(header+16,m->saved.vstart);undo_u64(header+24,m->saved.vlen);undo_u32(header+32,m->used);
 undo_u64(header+56,((hfv_size_t)m->saved.vstart+m->saved.vlen)*512);
 memcpy(header+344,m->saved.mdb.drVN,sizeof(m->saved.mdb.drVN));undo_u32(header+376,m->saved.mdb.drCrDate);
 /* The native media adapter reserves large descriptor values, not CRT fds. */
 if(m->saved.fd>=0&&m->saved.fd<0x10000000) {
  HANDLE source=(HANDLE)_get_osfhandle(m->saved.fd);BY_HANDLE_FILE_INFORMATION info;LARGE_INTEGER size;
  if(source!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(source,&info)) {
   undo_u32(header+40,info.dwVolumeSerialNumber);undo_u32(header+44,info.nFileIndexLow);undo_u32(header+48,info.nFileIndexHigh);
   memcpy(header+64,&info.ftCreationTime,8);memcpy(header+72,&info.ftLastWriteTime,8);
   if(GetFileSizeEx(source,&size))undo_u64(header+56,size.QuadPart);
   if(GetFinalPathNameByHandleA(source,(char*)header+80,260,FILE_NAME_NORMALIZED)>=260)header[80]=0;
  }
 }
 undo_u32(header+36,undo_crc(header,sizeof(header)));
 if(!WriteFile(file,header,sizeof(header),&written,0)||written!=sizeof(header))ok=0;
 for(i=0;ok&&i<m->used;++i) {
  struct undo_block *b=&m->blocks[i];undo_u64(record,((hfv_size_t)m->saved.vstart+b->sector)*512);
  undo_u32(record+8,undo_crc(b->before,512));undo_u32(record+12,512);memcpy(record+16,b->before,512);
  if(!WriteFile(file,record,sizeof(record),&written,0)||written!=sizeof(record))ok=0;
 }
 if(ok&&!FlushFileBuffers(file))ok=0;
 if(!ok){FILE_DISPOSITION_INFO removePartial={TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&removePartial,sizeof(removePartial));}
 CloseHandle(file);return ok;
}
#endif
void b_quarantine_volume(hfsvol *vol,const char *message) {
 FREE(vol->failure_message);vol->failure_message=(char*)save_memory(message,strlen(message)+1);
 vol->flags|=HFS_WRITE_FAILED|HFS_READONLY;
}
static void quarantine_mutation(struct mutation *m,hfsvol *vol) {
 char message[512];
#ifdef WIN32
 char path[MAX_PATH];
 if(save_failed_undo(m,path))snprintf(message,sizeof(message),"Classic HFS rollback failed; writes disabled. Undo records saved to \"%s\". Repair before writing.",path);
 else
#endif
 snprintf(message,sizeof(message),"Classic HFS rollback failed; writes disabled. Undo records could not be saved. Repair before writing.");
 b_quarantine_volume(vol,message);
}
int b_end_mutation(hfsvol *vol,int result) {
 struct mutation *m=active_mutation;unsigned i;int rollback=0,saved_errno;char *saved_error;block verify;
 if(!m||m->volume!=vol){ERROR(EIO,"Missing HFS undo state");return -1;}
 if(result==0&&hfs_flush_raw(vol)<0)result=-1;
 if(result==0&&m->used&&fhook_sync(vol->fd)<0){ERROR(errno,"Error flushing HFS device");result=-1;}
 saved_errno=errno;saved_error=hfs_error;active_mutation=0;
 if(result<0) {
  for(i=m->used;i>0;--i) {
   struct undo_block *b=&m->blocks[i-1];
   if(putblock(vol,b->sector,&b->before)<0||getblock(vol,b->sector,&verify)<0||memcmp(verify,b->before,HFS_BLOCKSZ))rollback=-1;
  }
  if(m->used&&fhook_sync(vol->fd)<0)rollback=-1;
  /* Restore deep-owned maps as well as the B-tree headers, bitmap and cache.
     The copied cache links still refer to this same original cache allocation. */
  FREE(vol->ext.map);FREE(vol->cat.map);
  *vol=m->saved;vol->ext.map=m->extmap;vol->cat.map=m->catmap;m->extmap=m->catmap=0;
  if(m->bitmap)memcpy(vol->vbm,m->bitmap,SIZE(block,ClassicHfsBitmapSectors(vol->mdb.drNmAlBlks)));
  if(m->cache)memcpy(vol->cache,m->cache,sizeof(bcache));
  for(i=0;i<m->file_count;++i)*m->files[i].file=m->files[i].saved;
  if(m->used)hfs_note_rollback(rollback?3:1);
  if(rollback)quarantine_mutation(m,vol);
 }
 free_mutation(m);
 if(rollback){ERROR(EIO,vol->failure_message?vol->failure_message:"Classic HFS rollback failed; writes disabled");return -1;}
 errno=saved_errno;hfs_error=saved_error;return result;
}

/*
 * NAME:	putblock()
 * DESCRIPTION:	write a block to the physical device
 */
static
int putblock(hfsvol *vol, unsigned long bnum, block *bp)
{
  hfv_offset_t offset;
  if (!ClassicHfsSectorOffset(vol->vstart, vol->vlen, bnum, 1, &offset)) {
    ERROR(EIO, "classic HFS write outside volume");
    return -1;
  }
  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled");return -1;}
  if(remember_block(vol,bnum)<0)return -1;
#ifdef WIN32
  if(hfv_hfs_before_sector_write(vol->fd,offset)<0){
    ERROR(EIO,"cannot publish classic HFS sector undo");
    return -1;
  }
#endif

  if (ClassicHfsTransferSector(vol->fd, offset, bp, 1, lseek, read, write) < 0) {
    ERROR(errno, "cannot write complete classic HFS sector");
    return -1;
  }

#ifdef WIN32
  hfv_hfs_after_sector_write(vol->fd);
#endif
  return 0;
}

/*
 * NAME:	getbucket()
 * DESCRIPTION:	fill a bucket's block buffer
 */
static
int getbucket(hfsvol *vol, bucket *b)
{
  if (getblock(vol, b->bnum, b->data) < 0)
    return -1;

  b->flags |=  HFS_BLOCK_INUSE;
  b->flags &= ~HFS_BLOCK_DIRTY;

  return 0;
}

/*
 * NAME:	putbucket()
 * DESCRIPTION:	store a bucket's block buffer
 */
static int store_cached_sector(void *volume, unsigned long sector, block *data)
{
  return putblock((hfsvol *)volume, sector, data);
}

int b_flush(hfsvol *vol)
{
  return vol->cache ? ClassicHfsFlushCache(vol->cache->chain, 0, store_cached_sector, vol) : 0;
}

/*
 * NAME:	block->finish()
 * DESCRIPTION:	commit and free a volume's block cache
 */
int b_finish(hfsvol *vol)
{
  int result;

  if (vol->cache == 0)
    return 0;

# ifdef DEBUG
  b_debugcache(vol);
# endif

  result = ClassicHfsFlushCache(vol->cache->chain, 1, store_cached_sector, vol);

  FREE(vol->cache);
  vol->cache = 0;

  return result;
}

static int transfer_logical_sector(hfsvol *volume, unsigned long sector, block *bytes, int writing)
{
    hfv_offset_t offset;
    bucket *entry;
    int found;

    if (volume->flags & HFS_WRITE_FAILED) {
        ERROR(EIO, "Reopen/repair HFS after rollback failure");
        return -1;
    }
    if (writing && (volume->flags & HFS_READONLY)) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if (!ClassicHfsSectorOffset(volume->vstart, volume->vlen, sector, writing, &offset)) {
        ERROR(EIO, "logical sector outside classic HFS volume");
        return -1;
    }
    if (!volume->cache)
        return writing ? putblock(volume, sector, bytes) : getblock(volume, sector, bytes);

    found = ClassicHfsFindCacheEntry(volume->cache, sector, &entry, store_cached_sector, volume);
    if (found < 0)
        return -1;
    if (writing) {
        memcpy(entry->data, bytes, HFS_BLOCKSZ);
        entry->flags |= HFS_BLOCK_INUSE | HFS_BLOCK_DIRTY;
    } else {
        if (!found && getbucket(volume, entry) < 0)
            return -1;
        memcpy(bytes, entry->data, HFS_BLOCKSZ);
    }
    return 0;
}

int ClassicHfsReadSector(hfsvol *volume, unsigned long sector, block *bytes)
{
    return transfer_logical_sector(volume, sector, bytes, 0);
}

int ClassicHfsWriteSector(hfsvol *volume, unsigned long sector, block *bytes)
{
    return transfer_logical_sector(volume, sector, bytes, 1);
}

int ClassicHfsTransferAllocationSector(hfsvol *volume, unsigned number, unsigned index,
                                       block *bytes, int writing)
{
    uint32_t sector;
    if (!ClassicHfsAllocationSector(volume->mdb.drAlBlSt, volume->mdb.drNmAlBlks,
                                    volume->lpa, volume->vlen, number, index, &sector)) {
        ERROR(EIO, "allocation sector outside classic HFS volume");
        return -1;
    }
    if (writing && (volume->flags & HFS_READONLY)) {
        ERROR(EROFS, "classic HFS volume is read-only");
        return -1;
    }
    if ((writing && !volume->vbm) || (volume->vbm && !ClassicHfsBitIsSet(volume->vbm, number))) {
        ERROR(EIO, "allocation sector is not marked in the classic HFS bitmap");
        return -1;
    }
    if (!writing)
        return ClassicHfsReadSector(volume, sector, bytes);
    if (ClassicHfsWriteSector(volume, sector, bytes) < 0)
        return -1;
    volume->mdb.drAtrb &= ~HFS_ATRB_UMOUNTED;
    volume->mdb.drLsMod = ClassicHfsFromUnixSeconds(time(0));
    ++volume->mdb.drWrCnt;
    volume->flags |= HFS_UPDATE_MDB;
    return 0;
}
