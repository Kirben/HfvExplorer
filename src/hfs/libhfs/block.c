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
# include "data.h"
# include "block.h"
# include "low.h"

#include "..\..\filehook.h"

/*
 * NAME:	block->init()
 * DESCRIPTION:	initialize a volume's block cache
 */
int b_init(hfsvol *vol)
{
  bcache *cache;
  int i;

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
  cache->vol = vol;

  for (i = 0; i < HFS_CACHESZ; ++i)
    {
      bucket *b = &cache->chain[i];

      b->flags = 0;
      b->count = 0;

      b->bnum  = 0;
      b->data  = &cache->pool[i];

      b->cnext = b + 1;
      b->cprev = b - 1;

      b->hnext = 0;
      b->hprev = 0;
    }

  cache->tail = &cache->chain[HFS_CACHESZ - 1];

  cache->chain[0].cprev = cache->tail;
  cache->tail->cnext    = &cache->chain[0];

  for (i = 0; i < HFS_HASHSZ; ++i)
    {
      bucket *b = &cache->hash[i];

      b->flags = 0;
      b->count = 0;

      b->bnum  = 0;
      b->data  = 0;

      b->cnext = 0;
      b->cprev = 0;

      b->hnext = 0;
      b->hprev = 0;
    }

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

	  printf(":%u", b->count);
	}
    }

  printf("\n");

  printf("HASH DUMP:\n");

  for (i = 0; i < HFS_HASHSZ; ++i)
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

	      printf(":%u", b->count);
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
  int bytes;

# ifdef DEBUG
  printf("READ: vol 0x%x block %lu\n", (unsigned int) vol, bnum);
# endif

  if (lseek(vol->fd, ((hfv_offset_t)vol->vstart + bnum) * HFS_BLOCKSZ, SEEK_SET) < 0)
    {
      ERROR(errno, "error seeking device");
      return -1;
    }

  bytes = read(vol->fd, bp, HFS_BLOCKSZ);

  if (bytes < 0)
    {
      ERROR(errno, "error reading from device");
      return -1;
    }
  else if (bytes == 0)
    {
      ERROR(EIO, "read EOF on volume");
      return -1;
    }
  else if (bytes != HFS_BLOCKSZ)
    {
      ERROR(EIO, "read incomplete block");
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
static struct mutation *active_mutation;
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
 bitmap_size=SIZE(block,(vol->mdb.drNmAlBlks+4095)/4096);
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
 if(hfs_flush(vol)<0){b_end_mutation(vol,-1);return -1;}
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
int b_end_mutation(hfsvol *vol,int result) {
 struct mutation *m=active_mutation;unsigned i;int rollback=0,saved_errno;char *saved_error;block verify;
 if(!m||m->volume!=vol){ERROR(EIO,"Missing HFS undo state");return -1;}
 if(result==0&&hfs_flush(vol)<0)result=-1;
 saved_errno=errno;saved_error=hfs_error;active_mutation=0;
 if(result<0) {
  for(i=m->used;i>0;--i) {
   struct undo_block *b=&m->blocks[i-1];
   if(putblock(vol,b->sector,&b->before)<0||getblock(vol,b->sector,&verify)<0||memcmp(verify,b->before,HFS_BLOCKSZ))rollback=-1;
  }
  /* Restore deep-owned maps as well as the B-tree headers, bitmap and cache.
     The copied cache links still refer to this same original cache allocation. */
  FREE(vol->ext.map);FREE(vol->cat.map);
  *vol=m->saved;vol->ext.map=m->extmap;vol->cat.map=m->catmap;m->extmap=m->catmap=0;
  if(m->bitmap)memcpy(vol->vbm,m->bitmap,SIZE(block,(vol->mdb.drNmAlBlks+4095)/4096));
  if(m->cache)memcpy(vol->cache,m->cache,sizeof(bcache));
  for(i=0;i<m->file_count;++i)*m->files[i].file=m->files[i].saved;
  if(rollback)vol->flags|=HFS_WRITE_FAILED|HFS_READONLY;
 }
 free_mutation(m);
 if(rollback){ERROR(EIO,"Classic HFS rollback failed; writes disabled; close and repair the image");return -1;}
 errno=saved_errno;hfs_error=saved_error;return result;
}

/*
 * NAME:	putblock()
 * DESCRIPTION:	write a block to the physical device
 */
static
int putblock(hfsvol *vol, unsigned long bnum, block *bp)
{
  int bytes;
  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled");return -1;}
  if(remember_block(vol,bnum)<0)return -1;

# ifdef DEBUG
  printf("WRITE: vol 0x%x block %lu\n", (unsigned int) vol, bnum);
# endif

  if (lseek(vol->fd, ((hfv_offset_t)vol->vstart + bnum) * HFS_BLOCKSZ, SEEK_SET) < 0)
    {
      ERROR(errno, "error seeking device");
      return -1;
    }

  bytes = write(vol->fd, bp, HFS_BLOCKSZ);

  if (bytes < 0)
    {
      ERROR(errno, "error writing to device");
      return -1;
    }
  else if (bytes != HFS_BLOCKSZ)
    {
      ERROR(EIO, "wrote incomplete block");
      return -1;
    }

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
static
int putbucket(hfsvol *vol, bucket *b)
{
  if (b->flags & HFS_BLOCK_INUSE &&
      b->flags & HFS_BLOCK_DIRTY)
    {
      if (putblock(vol, b->bnum, b->data) < 0)
	return -1;

      b->flags &= ~HFS_BLOCK_DIRTY;
    }

  return 0;
}

/*
 * NAME:	block->flush()
 * DESCRIPTION:	commit dirty cache blocks to a volume
 */
int b_flush(hfsvol *vol)
{
  bcache *cache = vol->cache;
  int i, result = 0;

  if (cache == 0)
    return 0;

  for (i = 0; i < HFS_CACHESZ; ++i)
    {
      if (putbucket(vol, &cache->chain[i]) < 0)
	result = -1;
    }

  return result;
}

/*
 * NAME:	compare()
 * DESCRIPTION:	comparison function for qsort of cache buckets
 */
static
int compare(bucket *b1, bucket *b2)
{
  return b1->bnum - b2->bnum;
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

  qsort(vol->cache->chain, HFS_CACHESZ, sizeof(bucket),
	(int (*)(const void *, const void *)) compare);

  result = b_flush(vol);

  FREE(vol->cache);
  vol->cache = 0;

  return result;
}

/*
 * NAME:	findbucket()
 * DESCRIPTION:	search for a cache bucket in the hash table
 */
static
int findbucket(bcache *cache, unsigned long bnum, bucket **ib)
{
  bucket *hb, *b, *p;
  int result;

  hb = &cache->hash[bnum & (HFS_HASHSZ - 1)];

  for (b = hb->hnext; b; b = b->hnext)
    {
      if (b->flags & HFS_BLOCK_INUSE &&
	  b->bnum == bnum)
	break;
    }

  if (b)
    {
      result = 1;

      /* cache hit; move towards head of cache chain */

      if (++b->count > b->cprev->count &&
	  b != cache->tail->cnext)
	{
	  p = b->cprev;

	  p->cprev->cnext = b;
	  b->cnext->cprev = p;

	  p->cnext = b->cnext;
	  b->cprev = p->cprev;

	  p->cprev = b;
	  b->cnext = p;

	  if (cache->tail == b)
	    cache->tail = p;
	}
    }
  else
    {
      result = 0;

      /* cache miss; reuse least-used cache bucket */

      b = cache->tail;

# ifdef DEBUG
      if (b->flags & HFS_BLOCK_INUSE)
	printf("CACHE: reusing bucket containing block %lu:%u\n",
	       b->bnum, b->count);
# endif

      if (putbucket(cache->vol, b) < 0)
	return -1;

      b->flags &= ~HFS_BLOCK_INUSE;
      b->count  = 1;
      b->bnum   = bnum;

      b->cnext->cprev = b->cprev;
      b->cprev->cnext = b->cnext;

      cache->tail = b->cprev;

      for (p = b->cnext; p->count > 1; p = p->cnext)
	--p->count;

      b->cprev = p->cprev;
      b->cnext = p;

      p->cprev->cnext = b;
      p->cprev = b;
    }

  /* insert at front of hash chain */

  if (hb->hnext != b)
    {
      if (b->hnext)
	b->hnext->hprev = b->hprev;
      if (b->hprev)
	b->hprev->hnext = b->hnext;

      b->hprev = hb;
      b->hnext = hb->hnext;

      hb->hnext = b;
      if (b->hnext)
	b->hnext->hprev = b;
    }

  *ib = b;

  return result;
}

/*
 * NAME:	block->readlb()
 * DESCRIPTION:	read a logical block from a volume (or from the cache)
 */
int b_readlb(hfsvol *vol, unsigned long bnum, block *bp)
{
  int found;
  bucket *b;
  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Reopen/repair HFS after rollback failure");return -1;}

  if (vol->cache == 0)
    return getblock(vol, bnum, bp);

  found = findbucket(vol->cache, bnum, &b);
  if (found < 0)
    return -1;

  if (! found &&
      getbucket(vol, b) < 0)
    return -1;

  memcpy(bp, b->data, HFS_BLOCKSZ);

  return 0;
}

/*
 * NAME:	block->writelb()
 * DESCRIPTION:	write a logical block to a volume (or to the cache)
 */
int b_writelb(hfsvol *vol, unsigned long bnum, block *bp)
{
  bucket *b;
  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled");return -1;}

  if (vol->cache == 0)
    return putblock(vol, bnum, bp);

  if (findbucket(vol->cache, bnum, &b) < 0)
    return -1;

  memcpy(b->data, bp, HFS_BLOCKSZ);
  b->flags |= HFS_BLOCK_INUSE | HFS_BLOCK_DIRTY;

  return 0;
}

/*
 * NAME:	block->readab()
 * DESCRIPTION:	read a block from an allocation block from a volume
 */
int b_readab(hfsvol *vol,
	     unsigned int anum, unsigned int index, block *bp)
{
  /* verify the allocation block exists and is marked as in-use */

  if (anum >= vol->mdb.drNmAlBlks)
    {
      ERROR(EIO, "read nonexistent block");
      return -1;
    }
  else if (vol->vbm && ! BMTST(vol->vbm, anum))
    {
      ERROR(EIO, "read unallocated block");
      return -1;
    }

  return b_readlb(vol, vol->mdb.drAlBlSt + anum * vol->lpa + index, bp);
}

/*
 * NAME:	b->writeab()
 * DESCRIPTION:	write a block to an allocation block to a volume
 */
int b_writeab(hfsvol *vol,
	      unsigned int anum, unsigned int index, block *bp)
{
  /* verify the allocation block exists and is marked as in-use */

  if (anum >= vol->mdb.drNmAlBlks)
    {
      ERROR(EIO, "write nonexistent block");
      return -1;
    }
  else if (vol->vbm && ! BMTST(vol->vbm, anum))
    {
      ERROR(EIO, "write unallocated block");
      return -1;
    }

  vol->mdb.drAtrb &= ~HFS_ATRB_UMOUNTED;
  vol->mdb.drLsMod = d_tomtime(time(0));
  ++vol->mdb.drWrCnt;

  vol->flags |= HFS_UPDATE_MDB;

  return b_writelb(vol, vol->mdb.drAlBlSt + anum * vol->lpa + index, bp);
}
