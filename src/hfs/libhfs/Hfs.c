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

/* Modified for emx by Marcus Better, July 1997 */

# include <stdlib.h>

#ifdef WIN32
# include <io.h>
# include <stdio.h>
#else
# include <unistd.h>
#endif

# include <fcntl.h>
# include <errno.h>
# include <string.h>
# include <time.h>
# include <ctype.h>
#ifdef __EMX__
# include <sys/types.h>
#endif
# include <sys/stat.h>

#ifdef __EMX__
#define INCL_DOSERRORS
#define INCL_DOSFILEMGR

#ifndef WIN32
#include <os2.h>
#include "os2open.h"
#endif

#endif

# include "../ClassicHfsCatalogKey.h"
#include "../ClassicHfsGeometry.h"
#include "../ClassicHfsForkIO.h"
# include "internal.h"
#ifdef WIN32
# include "../../ClassicHfsRecovery.h"
#endif
#include "../ClassicHfsTime.h"
#include "../ClassicHfsBytes.h"
# include "block.h"
# include "low.h"
#include "../ClassicHfsVolumeBitmap.h"
# include "file.h"
#include "../ClassicHfsSystemFiles.h"
#include "../ClassicHfsFormatTree.h"
# include "btree.h"
# include "node.h"
#include "../ClassicHfsEntry.h"
# include "volume.h"

#include "..\..\filehook.h"

/* Explicit mount/error contexts survive adapter migration between workers. */
static HFV_THREAD_LOCAL struct hfs_engine_context fallback_context;
static HFV_THREAD_LOCAL struct hfs_engine_context *active_context;
struct hfs_engine_context *hfs_context_get(void) {
 return active_context ? active_context : &fallback_context;
}
struct hfs_engine_context *hfs_context_use(struct hfs_engine_context *context) {
 struct hfs_engine_context *previous=active_context;active_context=context;return previous;
}

/* High-Level Volume Routines ============================================== */

int hfs_recognizes_image_fd(int fd) {
  hfsvol vol;
  memset(&vol,0,sizeof(vol));
  vol.fd=fd;
  vol.pnum=1;
  vol.flags=HFS_READONLY;
  return l_readblock0(&vol)==0 && l_readmdb(&vol)==0 && vol.mdb.drSigWord==HFS_SIGWORD;
}

/*
 * NAME:	hfs->mount()
 * DESCRIPTION:	open an HFS volume; return volume descriptor or 0 (error)
 */
static hfsvol *mount_impl(char *path, int pnum, int flags, int session, int supplied_fd)
{
  struct _stat64 dev;
  hfsvol *vol = 0;
  int recovered = 0;

#ifndef WIN32
#ifdef __EMX__
  char new_path[10];
  
  /* Handle drive specifications for Win32 */
  if(_emx_env & 0x1000) { /* Check if running under rsx */
    if(_emx_rev >> 16 == 2) { /* Check for RSXNT */
      if(isalpha(path[0]) && path[1]==':' && path[2]==0) {
	strcpy(new_path, "\\\\.\\");
	strcat(new_path, path);
	path = new_path;
      }
    }
  }
#endif
#endif

  /* A recovery remount adopts the already locked descriptor. */
  if (supplied_fd < 0)
    {

#ifdef WIN32
  {
    hfsvol *check;
    for(check=hfs_mounts;check;check=check->next) {
      if(fhook_same_file(check->fd,path) && (check->pnum==0||check->pnum==pnum) &&
         (!(check->flags&HFS_READONLY) || !(flags&(O_WRONLY|O_RDWR)))) {
        vol=check;break;
      }
    }
  }
#else
  if (fhook_stat(path, &dev) >= 0)
	// if (stat(path, &dev) >= 0)

		{
      struct _stat64 mdev;
      hfsvol *check;

      for (check = hfs_mounts; check; check = check->next)
	{
	  if (_fstat64(check->fd, &mdev) >= 0 &&
	      mdev.st_dev == dev.st_dev &&
	      mdev.st_ino == dev.st_ino &&
	      (check->pnum == 0 || check->pnum == pnum))
	    {
	      /* verify compatible read/write mode */

	      if (((check->flags & HFS_READONLY) &&
		   ! (flags & O_WRONLY)) ||
		  (! (check->flags & HFS_READONLY) &&
		   (flags & (O_WRONLY | O_RDWR))))
		{
		  vol = check;
		  break;
		}
	    }
	}
    }

#endif

    }

  if (vol == 0)
    {
      vol = ALLOC(hfsvol, 1);
      if (vol == 0)
	{
	  if (supplied_fd >= 0) close(supplied_fd);
	  ERROR(ENOMEM, 0);
	  return 0;
	}

      vol->flags  = 0;
      vol->failure_message = 0;
      vol->pnum   = pnum;
      vol->vstart = 0;
      vol->vlen   = 0;
      vol->lpa    = 0;

      vol->cache  = 0;

      vol->vbm    = 0;
      vol->cwd    = HFS_CNID_ROOTDIR;

      vol->refs   = 0;
      vol->files  = 0;
      vol->dirs   = 0;

      vol->prev   = 0;
      vol->next   = 0;

      vol->ext.map     = 0;
      vol->ext.mapsz   = 0;
      vol->ext.flags   = 0;
      vol->ext.compare = ClassicHfsCompareExtentKeys;

      vol->cat.map     = 0;
      vol->cat.mapsz   = 0;
      vol->cat.flags   = 0;
      vol->cat.compare = ClassicHfsCompareCatalogKeys;

      /* open and lock the device */

      if (supplied_fd >= 0)
        {
          vol->fd = supplied_fd;
          if (!(flags & (O_WRONLY | O_RDWR)))
            vol->flags |= HFS_READONLY;
        }
      else if (flags & (O_WRONLY | O_RDWR))
	{
#ifdef __EMX__
	  vol->fd = open(path, O_BINARY | O_RDWR);
#else
	  vol->fd = open(path, O_RDWR);
#endif
	  if (vol->fd >= 0 && l_lockvol(vol) < 0)
	    {
	      close(vol->fd);
	      vol->fd = -2;
	    }
	}

      if (supplied_fd < 0 && (! (flags & (O_WRONLY | O_RDWR)) ||
	  (vol->fd < 0 &&
	   (errno == EROFS || errno == EACCES || errno == EAGAIN) &&
	   (flags & O_RDWR))))
	{
	  vol->flags |= HFS_READONLY;
#ifdef __EMX__
	  vol->fd = open(path, O_BINARY | O_RDONLY);
#else
	  vol->fd = open(path, O_RDONLY);
#endif
	  if (vol->fd >= 0 && l_lockvol(vol) < 0)
	    {
	      close(vol->fd);
	      vol->fd = -2;
	    }
	}

      if (vol->fd < 0)
	{
	  if (vol->fd != -2)
	    ERROR(errno, errno==EBUSY ? "image is in use by another storage session" : "error opening device");

	  v_destruct(vol);

	  return 0;
	}

#ifdef WIN32
      recovered = hfv_hfs_recover_image(vol->fd, !(vol->flags & HFS_READONLY));
      if (recovered < 0)
        {
          close(vol->fd);
          v_destruct(vol);
          ERROR(EIO,"Classic HFS recovery could not complete. Keep the image and reopen with exclusive writable access.");
          return 0;
        }
#endif

      /* find out what kind of media this is and read the MDB */

      if (l_readblock0(vol) < 0 ||
	  l_readmdb(vol) < 0)
	{
	  close(vol->fd);
	  v_destruct(vol);

	  return 0;
	}

      /* verify this is an HFS volume */

      if (vol->mdb.drSigWord != HFS_SIGWORD ||
          (session && (vol->mdb.drVCSize == 0x482b || vol->mdb.drVCSize == 0x4858)))
	{
	  close(vol->fd);
	  v_destruct(vol);

	  ERROR(EINVAL, "not a Macintosh HFS volume");
	  return 0;
	}

      /* do minimal consistency checks */

      if (!vol->mdb.drAlBlkSiz || vol->mdb.drAlBlkSiz % HFS_BLOCKSZ != 0)
	{
	  close(vol->fd);
	  v_destruct(vol);

	  ERROR(EINVAL, "bad volume allocation block size");
	  return 0;
	}

      if (vol->vlen == 0) {
          uint32_t sectors;
          if (!ClassicHfsVolumeSectors(vol->mdb.drAlBlSt, vol->mdb.drNmAlBlks,
                                       vol->mdb.drAlBlkSiz >> HFS_BLOCKSZ_BITS, &sectors)) {
              close(vol->fd);
              v_destruct(vol);
              ERROR(EINVAL, "invalid classic HFS volume length");
              return 0;
          }
          vol->vlen = sectors;
      }

      /* initialize volume block cache (OK to fail) */

      b_init(vol);

      /* read the volume bitmap and extents/catalog B*-tree headers */

      if (ClassicHfsLoadVolumeBitmap(vol) < 0 ||
	  bt_readhdr(&vol->ext) < 0 ||
	  bt_readhdr(&vol->cat) < 0)
	{
	  close(vol->fd);
	  v_destruct(vol);
	  return 0;
	}

      if (! (vol->mdb.drAtrb & HFS_ATRB_UMOUNTED))
	{
          if (session && !recovered)
            vol->flags |= HFS_READONLY;
	  /* volume was not cleanly unmounted; scavenge free-space */

	  if (v_scavenge(vol) < 0)
	    {
	      close(vol->fd);
	      v_destruct(vol);
	      return 0;
	    }
	}

      if (vol->flags & HFS_READONLY)
	vol->mdb.drAtrb |= HFS_ATRB_HLOCKED;
      else
	vol->mdb.drAtrb &= ~HFS_ATRB_HLOCKED;

      vol->prev = 0;
      vol->next = hfs_mounts;

      if (hfs_mounts)
	hfs_mounts->prev = vol;

      hfs_mounts = vol;
    }

  return hfs_retain_mount(vol);
}

hfsvol *hfs_retain_mount(hfsvol *vol)
{
  ++vol->refs;
  return hfs_curvol = vol;
}

static HFV_THREAD_LOCAL int session_mounts;
int hfs_session_mounts(int enabled) {
  int previous=session_mounts;
  session_mounts=enabled;
  return previous;
}
hfsvol *hfs_mount(char *path, int pnum, int flags) {
  hfsvol *vol=mount_impl(path,pnum,flags,session_mounts,-1);
  if (session_mounts && vol && !(vol->flags&HFS_READONLY) && (vol->mdb.drAtrb&HFS_ATRB_UMOUNTED)) {
    if (hfs_mark_session_dirty(vol)<0) {
      hfs_umount(vol);
      ERROR(EIO,"Cannot mark classic HFS image dirty before writing");
      return 0;
    }
  }
  return vol;
}

hfsvol *hfs_mount_session(char *path, int pnum, int flags) {
  return mount_impl(path,pnum,flags,1,-1);
}

/* Takes ownership of an open image descriptor with its sharing lock held. */
hfsvol *hfs_mount_image_fd(int fd, int pnum, int flags, int session) {
  if (fd < 0) { ERROR(EBADF, "invalid HFS image descriptor"); return 0; }
  return mount_impl(0,pnum,flags,session,fd);
}

/* Takes ownership of a duplicate of the image's writable descriptor. */
hfsvol *hfs_remount_for_recovery(int fd, int pnum, int session) {
  if (fd < 0) { ERROR(EBADF, "invalid HFS recovery descriptor"); return 0; }
  return mount_impl(0,pnum,O_RDWR,session,fd);
}

/* Publish and flush the dirty flag before any non-journaled session write. */
int hfs_mark_session_dirty(hfsvol *vol) {
  if (b_begin_mutation(vol)<0) return -1;
  vol->mdb.drAtrb &= ~HFS_ATRB_UMOUNTED;
  vol->flags |= HFS_UPDATE_MDB;
  return b_end_mutation(vol,0);
}

/* Publish a clean marker while the caller still owns the durable undo log. */
int hfs_mark_session_clean(hfsvol *vol) {
  if (hfs_flush(vol)<0 || b_begin_mutation(vol)<0) return -1;
  return b_end_mutation(vol,v_flush(vol,1));
}

/*
 * NAME:	hfs->flush()
 * DESCRIPTION:	flush all pending changes to an HFS volume
 */
int hfs_flush_raw(hfsvol *vol)
{
  hfsfile *file;

  if (v_getvol(&vol) < 0)
    return -1;

  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled after rollback failure");return -1;}

  for (file = vol->files; file; file = file->next)
    {
      if (f_flush(file) < 0)
	return -1;
    }

  if (v_flush(vol, 0) < 0 ||
      b_flush(vol) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	hfs->flushall()
 * DESCRIPTION:	flush all pending changes to all mounted HFS volumes
 */
void hfs_flushall(void)
{
  hfsvol *vol;

  for (vol = hfs_mounts; vol; vol = vol->next)
    hfs_flush(vol);
}

/*
 * NAME:	hfs->umount()
 * DESCRIPTION:	close an HFS volume
 */
int hfs_umount(hfsvol *vol)
{
  int result = 0, saved_errno = 0;
  char *saved_error = 0;
  if (v_getvol(&vol) < 0) return -1;
  if(vol->flags&HFS_WRITE_FAILED) {
    result=-1;saved_errno=EIO;saved_error=vol->failure_message?vol->failure_message:"Classic HFS rollback failed; volume was not cleanly closed";
  }
  if (--vol->refs) return hfs_flush(vol);

  /* A rolled-back close keeps its file view for retry. Bound shutdown
     retries so a persistent read/validation error cannot hang unmount. */
  while(vol->files) {
    hfsfile *file=vol->files;
    if(hfs_close(file)<0) {
      if(result==0){result=-1;saved_errno=errno;saved_error=hfs_error;}
      if(vol->files==file&&hfs_close(file)<0&&vol->files==file) {
        b_quarantine_volume(vol,"Pending HFS file could not be finalized; writes disabled; repair before writing");
        hfs_close(file); // A quarantined close only releases memory.
      }
    }
  }
  if((vol->flags&HFS_WRITE_FAILED)&&vol->failure_message){saved_errno=EIO;saved_error=vol->failure_message;result=-1;}
  while (vol->dirs) hfs_closedir(vol->dirs);

  /* Commit pending sectors before publishing the clean-unmount marker.
     Keep the marker itself under undo and require its storage flush too. */
  if(result==0 && !(vol->flags&HFS_READONLY)) {
    if(hfs_flush(vol)<0) {
      result=-1;saved_errno=errno;saved_error=hfs_error;
    } else if(b_begin_mutation(vol)<0) {
      result=-1;saved_errno=errno;saved_error=hfs_error;
    } else {
      int marked=v_flush(vol,1);
      if(b_end_mutation(vol,marked)<0) {
        result=-1;saved_errno=errno;saved_error=hfs_error;
      }
    }
  }
  /* A failed finalization must not be retried outside undo by b_finish.
     Successful rollback has already restored and flushed the old sectors. */
  if(result!=0){FREE(vol->cache);vol->cache=0;}
  /* Always release the block cache, even after a metadata flush error. */
  if (b_finish(vol) < 0 && result == 0) {
    result = -1; saved_errno = errno; saved_error = hfs_error;
  }
  if (close(vol->fd) < 0 && result == 0) {
    result = -1; saved_errno = errno; saved_error = "error closing device";
  }
  if (vol->prev) vol->prev->next = vol->next;
  if (vol->next) vol->next->prev = vol->prev;
  if (vol == hfs_mounts) hfs_mounts = vol->next;
  if (vol == hfs_curvol) hfs_curvol = 0;
  if(saved_error && saved_error==vol->failure_message) {
    char *failure_after_close=hfs_context_get()->failure_after_close;
    strncpy(failure_after_close,saved_error,511);
    failure_after_close[511]=0;saved_error=failure_after_close;
  }
  v_destruct(vol);
  if (result) { errno = saved_errno; hfs_error = saved_error; }
  return result;
}

/*
 * NAME:	hfs->umountall()
 * DESCRIPTION:	unmount all mounted volumes
 */
void hfs_umountall(void)
{
  while (hfs_mounts)
    hfs_umount(hfs_mounts);
}

/*
 * NAME:	hfs->getvol()
 * DESCRIPTION:	return a pointer to a mounted volume
 */
hfsvol *hfs_getvol(char *name)
{
  hfsvol *vol;

  if (name == 0)
    return hfs_curvol;

  for (vol = hfs_mounts; vol; vol = vol->next)
    {
      if (ClassicHfsCompareNames(name, vol->mdb.drVN) == 0)
	return vol;
    }

  return 0;
}

/*
 * NAME:	hfs->setvol()
 * DESCRIPTION:	change the current volume
 */
void hfs_setvol(hfsvol *vol)
{
  hfs_curvol = vol;
}

/*
 * NAME:	hfs->vstat()
 * DESCRIPTION:	return volume statistics
 */
int hfs_vstat(hfsvol *vol, hfsvolent *ent)
{
  if (v_getvol(&vol) < 0)
    return -1;

  strcpy(ent->name, vol->mdb.drVN);

  ent->flags     = (vol->flags & HFS_READONLY) ? HFS_ISLOCKED : 0;

  ent->totbytes  = (hfv_size_t)vol->mdb.drNmAlBlks * vol->mdb.drAlBlkSiz;
  ent->freebytes = (hfv_size_t)vol->mdb.drFreeBks  * vol->mdb.drAlBlkSiz;

  ent->alblocksz = vol->mdb.drAlBlkSiz;
  ent->clumpsz   = vol->mdb.drClpSiz;

  ent->numfiles  = vol->mdb.drFilCnt;
  ent->numdirs   = vol->mdb.drDirCnt;

  ent->crdate    = ClassicHfsToUnixSeconds(vol->mdb.drCrDate);
  ent->mddate    = ClassicHfsToUnixSeconds(vol->mdb.drLsMod);
  ent->bkdate    = ClassicHfsToUnixSeconds(vol->mdb.drVolBkUp);

  return 0;
}

/*
 * NAME:	hfs->format()
 * DESCRIPTION:	write a new filesystem
 */
int hfs_format(char *path, int pnum, char *vname)
{
  hfsvol vol;
  btree *ext = &vol.ext;
  btree *cat = &vol.cat;
  ClassicHfsFormatGeometry geometry;
  size_t nameLength = vname ? strlen(vname) : 0;
  int result = 0;
  block vbm[16];

  if (vname && strchr(vname, ':'))
    {
      ERROR(EINVAL, "volume name may not contain colons");
      return -1;
    }

  if (!nameLength || nameLength > HFS_MAX_VLEN)
    {
      ERROR(EINVAL, "volume name must be 1-27 chars");
      return -1;
    }

  vol.flags  = 0;
  vol.failure_message = 0;
  vol.pnum   = pnum;
  vol.vstart = 0;
  vol.vlen   = 0;
  vol.lpa    = 0;

  vol.cache  = 0;

  vol.vbm    = vbm;
  vol.cwd    = HFS_CNID_ROOTDIR;

  vol.refs   = 0;
  vol.files  = 0;
  vol.dirs   = 0;

  vol.prev   = 0;
  vol.next   = 0;

#ifdef __EMX__
  vol.fd = open(path, O_BINARY | O_RDWR);
#else
  vol.fd = open(path, O_RDWR);
#endif
  if (vol.fd < 0)
    {
      ERROR(errno, "error opening device for writing");
      return -1;
    }

  if (l_lockvol(&vol) < 0)
    {
      close(vol.fd);
      return -1;
    }

  if (pnum > 0)
    {
      if (l_readpm(&vol) < 0)
	{
	  close(vol.fd);
	  return -1;
	}
    }
  else  /* determine size of entire device */
    {
      unsigned long low, high, mid;
      block b;

      for (low = 0, high = 2879; b_readlb(&vol, high, &b) >= 0; ) {
        low = high;
        if (high == 0xffffffffUL) { close(vol.fd); ERROR(EFBIG, "image exceeds classic HFS sector range"); return -1; }
        high = high > 0xffffffffUL / 2 ? 0xffffffffUL : high * 2;
      }

      while (low < high - 1)
	{
	  mid = low + (high - low) / 2;

	  if (b_readlb(&vol, mid, &b) < 0)
	    high = mid;
	  else
	    low = mid;
	}

      vol.vlen = low + 1;
    }

  if (!ClassicHfsPlanFormat(vol.vlen, &geometry))
    {
      close(vol.fd);

      ERROR(EINVAL, "volume size must be >= 800K");
      return -1;
    }

  /* initialize volume block cache (OK to fail) */

  b_init(&vol);

  /* initialize volume geometry */

  vol.lpa = geometry.sectorsPerBlock;

  ClassicHfsInitMasterDirectory(&vol.mdb, &geometry, vname, nameLength,
                                ClassicHfsFromUnixSeconds(time(0)));

  /* initialize volume bitmap */

  memset(vol.vbm, 0, sizeof(vbm));

  ClassicHfsInitSystemFiles(&vol);

  cat->map = NULL; // cleanup also runs if the first allocation fails
  if (ClassicHfsInitFormatTree(ext, ClassicHfsExtentKeySize - 1, ClassicHfsCompareExtentKeys) < 0 ||
      ClassicHfsInitFormatTree(cat, 37, ClassicHfsCompareCatalogKeys) < 0 ||
      bt_space(ext, 1) < 0 || bt_space(cat, 1) < 0)
    result = -1;

  /* create root folder */

  if (result == 0 && v_newfolder(&vol, HFS_CNID_ROOTPAR, vname) < 0)
    result = -1;

  vol.mdb.drNxtCNID = 16;

  /* finish up */

  if (result == 0)
    {
      block b;

      /* write boot blocks */

      memset(&b, 0, sizeof(b));
      if (b_writelb(&vol, 0, &b) < 0 || b_writelb(&vol, 1, &b) < 0)
	result = -1;
    }

  if (result == 0)
    {
      vol.flags |= HFS_UPDATE_MDB | HFS_UPDATE_ALTMDB | HFS_UPDATE_VBM;
      if (v_flush(&vol, 1) < 0 || b_finish(&vol) < 0)
	result = -1;
    }
  FREE(vol.cache);

  if (close(vol.fd) < 0 && result == 0)
    {
      ERROR(errno, "error closing device");
      result = -1;
    }

  FREE(vol.ext.map);
  FREE(vol.cat.map);

  return result;
}

/* High-Level Directory Routines =========================================== */

/*
 * NAME:	hfs->chdir()
 * DESCRIPTION:	change current HFS directory
 */
int hfs_chdir(hfsvol *vol, char *path)
{
  CatDataRec data;

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, path, &data, 0, 0, 0) <= 0)
    return -1;

  if (data.cdrType != cdrDirRec)
    {
      ERROR(ENOTDIR, 0);
      return -1;
    }

  vol->cwd = data.u.dir.dirDirID;

  return 0;
}

/*
 * NAME:	hfs->getcwd()
 * DESCRIPTION:	return the current working directory ID
 */
long hfs_getcwd(hfsvol *vol)
{
  if (v_getvol(&vol) < 0)
    return 0;

  return vol->cwd;
}

/*
 * NAME:	hfs->setcwd()
 * DESCRIPTION:	set the current working directory ID
 */
int hfs_setcwd(hfsvol *vol, long id)
{
  if (v_getvol(&vol) < 0)
    return -1;

  if (id == vol->cwd)
    return 0;

  /* make sure the directory exists */

  if (v_getdthread(vol, id, 0, 0) <= 0)
    return -1;

  vol->cwd = id;

  return 0;
}

/*
 * NAME:	hfs->dirinfo()
 * DESCRIPTION:	given a directory ID, return its (name and) parent ID
 */
int hfs_dirinfo(hfsvol *vol, long *id, char *name)
{
  CatDataRec thread;

  if (v_getvol(&vol) < 0 ||
      v_getdthread(vol, *id, &thread, 0) <= 0)
    return -1;

  *id = thread.u.dthd.thdParID;

  if (name)
    strcpy(name, thread.u.dthd.thdCName);

  return 0;
}

/*
 * NAME:	hfs->opendir()
 * DESCRIPTION:	prepare to read the contents of a directory
 */
hfsdir *hfs_opendir(hfsvol *vol, char *path)
{
  hfsdir *dir;
  CatDataRec data;
  unsigned char pkey[HFS_CATKEYLEN];

  if (v_getvol(&vol) < 0)
    return 0;

  dir = ALLOC(hfsdir, 1);
  if (dir == 0)
    {
      ERROR(ENOMEM, 0);
      return 0;
    }


  if (*path == 0)
    {
      /* meta-directory containing root dirs from all mounted volumes */

      dir->dirid = 0;
      dir->vptr  = hfs_mounts;
    }
  else
    {
      if (v_resolve(vol, path, &data, 0, 0, 0) <= 0)
	{
	  FREE(dir);
	  return 0;
	}

      if (data.cdrType != cdrDirRec)
	{
	  FREE(dir);
	  ERROR(ENOTDIR, 0);
	  return 0;
	}

      dir->dirid = data.u.dir.dirDirID;
      dir->vptr  = 0;

      ClassicHfsWriteCatalogKey(pkey, dir->dirid, "");

      if (bt_search(&vol->cat, pkey, &dir->n) <= 0)
	{
	  FREE(dir);
	  return 0;
	}
    }

  /* Path resolution may select another mounted volume. Link and close the
     directory through that resolved volume, not the caller's original one. */
  dir->vol = vol;
  dir->prev = 0;
  dir->next = vol->dirs;

  if (vol->dirs)
    vol->dirs->prev = dir;

  vol->dirs = dir;

  return dir;
}

/*
 * NAME:	hfs->readdir()
 * DESCRIPTION:	return the next entry in the directory
 */
int hfs_readdir(hfsdir *dir, hfsdirent *ent)
{
  CatKeyRec key;
  CatDataRec data;
  unsigned char *ptr;

  if (dir->dirid == 0)
    {
      hfsvol *vol;
      char cname[HFS_MAX_FLEN + 1];

      for (vol = hfs_mounts; vol; vol = vol->next)
	{
	  if (vol == dir->vptr)
	    break;
	}

      if (vol == 0)
	{
	  ERROR(ENOENT, "no more entries");
	  return -1;
	}

      if (v_getdthread(vol, HFS_CNID_ROOTDIR, &data, 0) <= 0 ||
	  v_catsearch(vol, HFS_CNID_ROOTPAR, data.u.dthd.thdCName,
		      &data, cname, 0) < 0)
	return -1;

      ClassicHfsDescribeEntry(HFS_CNID_ROOTPAR, cname, &data, ent);

      dir->vptr = vol->next;

      return 0;
    }

  if (dir->n.rnum == -1)
    {
      ERROR(ENOENT, "no more entries");
      return -1;
    }

  while (1)
    {
      ++dir->n.rnum;

      while (dir->n.rnum >= dir->n.nd.ndNRecs)
	{
	  dir->n.nnum = dir->n.nd.ndFLink;
	  if (dir->n.nnum == 0)
	    {
	      dir->n.rnum = -1;
	      ERROR(ENOENT, "no more entries");
	      return -1;
	    }

	  if (bt_getnode(&dir->n) < 0)
	    {
	      dir->n.rnum = -1;
	      return -1;
	    }

	  dir->n.rnum = 0;
	}

      ptr = HFS_NODEREC(dir->n, dir->n.rnum);

      int payload = ClassicHfsCatalogPayload(ptr, dir->n.roff[dir->n.rnum + 1] - dir->n.roff[dir->n.rnum], &key);
      if (!payload) {
        ERROR(EIO, "invalid catalog key");
        return -1;
      }

      if (key.ckrParID != dir->dirid)
	{
	  dir->n.rnum = -1;
	  ERROR(ENOENT, "no more entries");
	  return -1;
	}

      ClassicHfsDecodeCatalogRecord(ptr + payload, dir->n.roff[dir->n.rnum + 1] - dir->n.roff[dir->n.rnum] - payload, &data);

      switch (data.cdrType)
	{
	case cdrDirRec:
	case cdrFilRec:
	  ClassicHfsDescribeEntry(key.ckrParID, key.ckrCName, &data, ent);
	  return 0;

	case cdrThdRec:
	case cdrFThdRec:
	  break;

	default:
	  dir->n.rnum = -1;

	  ERROR(EIO, "unexpected directory entry found");
	  return -1;
	}
    }
}

// By lp. Same as hfs_readdir, but w/ extra parameters
int hfs_readdir2(
	hfsdir *dir, 
	hfsdirent *ent,
  CatKeyRec *pkey,
  CatDataRec *pdata
)
{
  unsigned char *ptr;

  if (dir->dirid == 0) {
	  hfsvol *vol;
		char cname[HFS_MAX_FLEN + 1];

    for (vol = hfs_mounts; vol; vol = vol->next) {
			if (vol == dir->vptr) break;
		}
		if (vol == 0) {
		  ERROR(ENOENT, "no more entries");
		  return -1;
		}

    if (v_getdthread(vol, HFS_CNID_ROOTDIR, pdata, 0) <= 0 ||
				v_catsearch(vol, HFS_CNID_ROOTPAR, pdata->u.dthd.thdCName,
					  pdata, cname, 0) < 0)
				return -1;

    ClassicHfsDescribeEntry(HFS_CNID_ROOTPAR, cname, pdata, ent);
    dir->vptr = vol->next;
    return 0;
  }
  if (dir->n.rnum == -1) {
		ERROR(ENOENT, "no more entries");
    return -1;
  }
  while(1) {
		++dir->n.rnum;
    while (dir->n.rnum >= dir->n.nd.ndNRecs) {
			dir->n.nnum = dir->n.nd.ndFLink;
			if (dir->n.nnum == 0) {
	      dir->n.rnum = -1;
	      ERROR(ENOENT, "no more entries");
	      return -1;
	    }
		  if (bt_getnode(&dir->n) < 0) {
	      dir->n.rnum = -1;
	      return -1;
	    }
		  dir->n.rnum = 0;
		}
    ptr = HFS_NODEREC(dir->n, dir->n.rnum);
    int payload = ClassicHfsCatalogPayload(ptr, dir->n.roff[dir->n.rnum + 1] - dir->n.roff[dir->n.rnum], pkey);
      if (!payload) {
      ERROR(EIO, "invalid catalog key");
      return -1;
    }
    if (pkey->ckrParID != dir->dirid) {
		  dir->n.rnum = -1;
		  ERROR(ENOENT, "no more entries");
			return -1;
		}
    ClassicHfsDecodeCatalogRecord(ptr + payload, dir->n.roff[dir->n.rnum + 1] - dir->n.roff[dir->n.rnum] - payload, pdata);
    switch (pdata->cdrType) {
			case cdrDirRec:
			case cdrFilRec:
				ClassicHfsDescribeEntry(pkey->ckrParID, pkey->ckrCName, pdata, ent);
				return 0;
			case cdrThdRec:
			case cdrFThdRec:
				break;
			default:
				dir->n.rnum = -1;
			  ERROR(EIO, "unexpected directory entry found");
				return -1;
		}
  }
}

/*
 * NAME:	hfs->closedir()
 * DESCRIPTION:	stop reading a directory
 */
int hfs_closedir(hfsdir *dir)
{
  hfsvol *vol = dir->vol;

  if (dir->prev)
    dir->prev->next = dir->next;
  if (dir->next)
    dir->next->prev = dir->prev;
  if (dir == vol->dirs)
    vol->dirs = dir->next;

  FREE(dir);

  return 0;
}

/* High-Level File Routines ================================================ */

/*
 * NAME:	hfs->open()
 * DESCRIPTION:	prepare a file for I/O
 */
hfsfile *hfs_open(hfsvol *vol, char *path)
{
  hfsfile *file;

  if (v_getvol(&vol) < 0)
    return 0;

  file = ALLOC(hfsfile, 1);
  if (file == 0)
    {
      ERROR(ENOMEM, 0);
      return 0;
    }

  if (v_resolve(vol, path, &file->cat, &file->parid, file->name, 0) <= 0)
    {
      FREE(file);
      return 0;
    }

  if (file->cat.cdrType != cdrFilRec)
    {
      FREE(file);
      ERROR(EISDIR, 0);
      return 0;
    }

  if (file->cat.u.fil.filLgLen > HFS_MAX_FORK_SIZE || file->cat.u.fil.filRLgLen > HFS_MAX_FORK_SIZE) {
    FREE(file); ERROR(EFBIG, "classic HFS fork exceeds Apple's 2 GiB minus one limit"); return 0;
  }
  file->vol   = vol;
  file->clump = file->cat.u.fil.filClpSize;
  file->flags = 0;

  ClassicHfsSelectFork(file, 0);

  file->prev = 0;
  file->next = vol->files;

  if (vol->files)
    vol->files->prev = file;

  vol->files = file;

  return file;
}

/*
 * NAME:	hfs->setfork()
 * DESCRIPTION:	select file fork for I/O operations
 */
int hfs_setfork(hfsfile *file, int fork)
{
  int result = 0;

  if (! (file->vol->flags & HFS_READONLY) &&
      f_trunc(file) < 0)
    result = -1;

  ClassicHfsSelectFork(file, fork);

  return result;
}

/*
 * NAME:	hfs->getfork()
 * DESCRIPTION:	return the current fork for I/O operations
 */
int hfs_getfork(hfsfile *file)
{
  return file->fork != fkData;
}

/*
 * NAME:	hfs->read()
 * DESCRIPTION:	read from an open file
 */
static int read_fork_block(void *context, uint32_t sector, block *bytes)
{
  return f_getblock((hfsfile *)context, sector, bytes);
}

long hfs_read(hfsfile *file, void *buf, unsigned long len)
{
  unsigned long *logicalLength;
  logicalLength = ClassicHfsSelectedFork(file).logical;
  return ClassicHfsReadFork(file, read_fork_block, *logicalLength, &file->pos, buf, len);
}

/*
 * NAME:	hfs->write()
 * DESCRIPTION:	write to an open file
 */
static int write_fork_block(void *context, uint32_t sector, block *bytes)
{
  return f_putblock((hfsfile *)context, sector, bytes);
}

static int grow_fork(void *context)
{
  hfsfile *file = (hfsfile *)context;
  if (bt_space(&file->vol->ext, 1) < 0)
    return -1;
  return f_alloc(file);
}

long hfs_write(hfsfile *file, void *buf, unsigned long len)
{
  unsigned long *lglen, *pylen;

  if (file->vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  ClassicHfsForkFields forkFields = ClassicHfsSelectedFork(file);
  lglen = forkFields.logical;
  pylen = forkFields.physical;

  if (len > 0x7fffffffUL) { ERROR(EINVAL, "transfer count exceeds signed API range"); return -1; }
  if (file->pos > HFS_MAX_FORK_SIZE || len > HFS_MAX_FORK_SIZE - file->pos) { ERROR(EFBIG, "classic HFS fork exceeds Apple limit"); return -1; }
  if (len)
    {
      file->cat.u.fil.filMdDat = ClassicHfsFromUnixSeconds(time(0));
      file->flags |= HFS_UPDATE_CATREC;
    }

  return ClassicHfsWriteFork(file, read_fork_block, write_fork_block, grow_fork,
                             pylen, lglen, &file->pos, buf, len);
}

/*
 * NAME:	hfs->truncate()
 * DESCRIPTION:	truncate an open file
 */
static int hfs_truncate_impl(hfsfile *file, unsigned long len)
{
  if (len > HFS_MAX_FORK_SIZE) { ERROR(EFBIG, "classic HFS fork exceeds Apple limit"); return -1; }
  unsigned long *lglen;

  lglen = ClassicHfsSelectedFork(file).logical;

  if (*lglen > len)
    {
      if (file->vol->flags & HFS_READONLY)
	{
	  ERROR(EROFS, 0);
	  return -1;
	}

      *lglen = len;

      file->cat.u.fil.filMdDat = ClassicHfsFromUnixSeconds(time(0));
      file->flags |= HFS_UPDATE_CATREC;

      if (file->pos > len)
	file->pos = len;
    }

  return 0;
}

/*
 * NAME:	hfs->lseek()
 * DESCRIPTION:	change file seek pointer
 */
hfv_offset_t hfs_lseek(hfsfile *file, hfv_offset_t offset, int from)
{
  unsigned long *lglen;
  lglen = ClassicHfsSelectedFork(file).logical;
  if (!ClassicHfsSeekFork(*lglen, &file->pos, offset, from)) {
    ERROR(EINVAL, 0);
    return -1;
  }
  return file->pos;
}

/*
 * NAME:	hfs->close()
 * DESCRIPTION:	close a file
 */
int hfs_close(hfsfile *file)
{
  hfsvol *vol=file->vol;int result=0;
  if(vol->flags&HFS_WRITE_FAILED)result=-1;
  else if(!(vol->flags&HFS_READONLY)) {
    unsigned long *logical,*physical;hfv_size_t rounded;
    ClassicHfsForkFields forkFields = ClassicHfsSelectedFork(file);
    logical = forkFields.logical;
    physical = forkFields.physical;
    rounded=((hfv_size_t)*logical+vol->mdb.drAlBlkSiz-1)/vol->mdb.drAlBlkSiz*vol->mdb.drAlBlkSiz;
    if((file->flags&HFS_UPDATE_CATREC)||*physical!=rounded) {
      if(b_begin_mutation(vol)<0) {
        if(!(vol->flags&HFS_WRITE_FAILED))return -1; // Keep the view for retry.
        result=-1;
      } else {
        if(f_trunc(file)<0||f_flush(file)<0)result=-1;
        result=b_end_mutation(vol,result);
        if(result<0&&!(vol->flags&HFS_WRITE_FAILED))return -1;
      }
    }
  }
  if(file->prev)file->prev->next=file->next;
  if(file->next)file->next->prev=file->prev;
  if(file==vol->files)vol->files=file->next;
  FREE(file);return result;
}

/* High-Level Catalog Routines ============================================= */

/*
 * NAME:	hfs->stat()
 * DESCRIPTION:	return catalog information for an arbitrary path
 */
int hfs_stat(hfsvol *vol, char *path, hfsdirent *ent)
{
  CatDataRec data;
  long parid;
  char name[HFS_MAX_FLEN + 1];

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, path, &data, &parid, name, 0) <= 0)
    return -1;

  ClassicHfsDescribeEntry(parid, name, &data, ent);

  return 0;
}

/*
 * NAME:	hfs->fstat()
 * DESCRIPTION:	return catalog information for an open file
 */
int hfs_fstat(hfsfile *file, hfsdirent *ent)
{
  ClassicHfsDescribeEntry(file->parid, file->name, &file->cat, ent);

  return 0;
}

/*
 * NAME:	hfs->setattr()
 * DESCRIPTION:	change a file's attributes
 */
static int hfs_setattr_impl(hfsvol *vol, char *path, hfsdirent *ent)
{
  CatDataRec data;
  node n;

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, path, &data, 0, 0, &n) <= 0)
    return -1;

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  ClassicHfsApplyEntry(&data, ent);

  if (v_putcatrec(&data, &n) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	hfs->fsetattr()
 * DESCRIPTION:	change an open file's attributes
 */
int hfs_fsetattr(hfsfile *file, hfsdirent *ent)
{
  if (file->vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  ClassicHfsApplyEntry(&file->cat, ent);

  file->flags |= HFS_UPDATE_CATREC;

  return 0;
}

/*
 * NAME:	hfs->mkdir()
 * DESCRIPTION:	create a new directory
 */
static int hfs_mkdir_impl(hfsvol *vol, char *path)
{
  CatDataRec data;
  long parid;
  char name[HFS_MAX_FLEN + 1];
  int found;

  if (v_getvol(&vol) < 0)
    return -1;

  found = v_resolve(vol, path, &data, &parid, name, 0);
  if (found < 0 || parid == 0)
    return -1;
  else if (found)
    {
      ERROR(EEXIST, 0);
      return -1;
    }

  if (parid == HFS_CNID_ROOTPAR)
    {
      ERROR(EINVAL, 0);
      return -1;
    }

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  if (v_newfolder(vol, parid, name) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	hfs->rmdir()
 * DESCRIPTION:	delete an empty directory
 */
static int hfs_rmdir_impl(hfsvol *vol, char *path)
{
  CatDataRec data;
  long parid;
  char name[HFS_MAX_FLEN + 1];
  unsigned char pkey[HFS_CATKEYLEN];

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, path, &data, &parid, name, 0) <= 0)
    return -1;

  if (data.cdrType != cdrDirRec)
    {
      ERROR(ENOTDIR, 0);
      return -1;
    }

  if (data.u.dir.dirVal != 0)
    {
      ERROR(ENOTEMPTY, 0);
      return -1;
    }

  if (parid == HFS_CNID_ROOTPAR)
    {
      ERROR(EINVAL, 0);
      return -1;
    }

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  /* delete directory record */

  if (ClassicHfsWriteCatalogKey(pkey, parid, name) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }

  if (bt_delete(&vol->cat, pkey) < 0)
    return -1;

  /* delete thread record */

  ClassicHfsWriteCatalogKey(pkey, data.u.dir.dirDirID, "");

  if (bt_delete(&vol->cat, pkey) < 0 ||
      v_adjvalence(vol, parid, 1, -1) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	hfs->create()
 * DESCRIPTION:	create a new file
 */
static int hfs_create_impl(hfsvol *vol, char *path, char *type, char *creator)
{
  CatDataRec data;
  long id, parid;
  char name[HFS_MAX_FLEN + 1];
  unsigned char record[HFS_CATRECMAXLEN];
  int found, i, reclen;

  if (v_getvol(&vol) < 0)
    return -1;

  found = v_resolve(vol, path, &data, &parid, name, 0);
  if (found < 0 || parid == 0)
    return -1;
  else if (found)
    {
      ERROR(EEXIST, 0);
      return -1;
    }

  if (parid == HFS_CNID_ROOTPAR)
    {
      ERROR(EINVAL, 0);
      return -1;
    }

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  /* create file `name' in parent `parid' */

  if (bt_space(&vol->cat, 1) < 0)
    return -1;

  id = vol->mdb.drNxtCNID++;
  vol->flags |= HFS_UPDATE_MDB;

  /* create file record */

  data.cdrType   = cdrFilRec;
  data.cdrResrv2 = 0;

  data.u.fil.filFlags = 0;
  data.u.fil.filTyp   = 0;

  memset(&data.u.fil.filUsrWds, 0, sizeof(data.u.fil.filUsrWds));

  data.u.fil.filUsrWds.fdType    = ClassicHfsRead32((unsigned char *) type);
  data.u.fil.filUsrWds.fdCreator = ClassicHfsRead32((unsigned char *) creator);

  data.u.fil.filFlNum  = id;
  data.u.fil.filStBlk  = 0;
  data.u.fil.filLgLen  = 0;
  data.u.fil.filPyLen  = 0;
  data.u.fil.filRStBlk = 0;
  data.u.fil.filRLgLen = 0;
  data.u.fil.filRPyLen = 0;
  data.u.fil.filCrDat  = ClassicHfsFromUnixSeconds(time(0));
  data.u.fil.filMdDat  = data.u.fil.filCrDat;
  data.u.fil.filBkDat  = 0;

  memset(&data.u.fil.filFndrInfo, 0, sizeof(data.u.fil.filFndrInfo));

  data.u.fil.filClpSize = 0;

  for (i = 0; i < 3; ++i)
    {
      data.u.fil.filExtRec[i].xdrStABN     = 0;
      data.u.fil.filExtRec[i].xdrNumABlks  = 0;

      data.u.fil.filRExtRec[i].xdrStABN    = 0;
      data.u.fil.filRExtRec[i].xdrNumABlks = 0;
    }

  data.u.fil.filResrv = 0;

  if ((reclen = ClassicHfsWriteCatalogKey(record, parid, name)) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }
  {
    unsigned size = ClassicHfsEncodeCatalogRecord(record + reclen, sizeof(record) - reclen, &data);
    if (!size) { ERROR(EIO, "invalid classic HFS catalog record size"); return -1; }
    reclen += size;
  }

  if (bt_insert(&vol->cat, record, reclen) < 0 ||
      v_adjvalence(vol, parid, 0, 1) < 0)
    return -1;

  return 0;
}

/*
 * NAME:	hfs->delete()
 * DESCRIPTION:	remove both forks of a file
 */
static int hfs_delete_impl(hfsvol *vol, char *path)
{
  hfsfile file;
  unsigned char pkey[HFS_CATKEYLEN];
  int found;

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, path, &file.cat, &file.parid, file.name, 0) <= 0)
    return -1;

  if (file.cat.cdrType != cdrFilRec)
    {
      ERROR(EISDIR, 0);
      return -1;
    }

  if (file.parid == HFS_CNID_ROOTPAR)
    {
      ERROR(EINVAL, 0);
      return -1;
    }

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  /* There is no orphan-inode mechanism: freeing an open fork would let
     later I/O overwrite blocks already reassigned to another file. */
  {
    hfsfile *opened;
    for (opened = vol->files; opened; opened = opened->next)
      if (opened->cat.u.fil.filFlNum == file.cat.u.fil.filFlNum) {
        ERROR(EBUSY, "file is open; close it before deleting");
        return -1;
      }
  }

  /* free disk blocks */

  file.vol   = vol;
  file.flags = 0;

  file.cat.u.fil.filLgLen  = 0;
  file.cat.u.fil.filRLgLen = 0;

  ClassicHfsSelectFork(&file, 0);
  if (f_trunc(&file) < 0)
    return -1;

  ClassicHfsSelectFork(&file, 1);
  if (f_trunc(&file) < 0)
    return -1;

  /* delete file record */

  if (ClassicHfsWriteCatalogKey(pkey, file.parid, file.name) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }

  if (bt_delete(&vol->cat, pkey) < 0 ||
      v_adjvalence(vol, file.parid, 0, -1) < 0)
    return -1;

  /* delete file thread, if any */

  found = v_getfthread(vol, file.cat.u.fil.filFlNum, 0, 0);
  if (found < 0)
    return -1;

  if (found)
    {
      ClassicHfsWriteCatalogKey(pkey, file.cat.u.fil.filFlNum, "");

      if (bt_delete(&vol->cat, pkey) < 0)
	return -1;
    }

  return 0;
}

/*
 * NAME:	hfs->rename()
 * DESCRIPTION:	change the name of and/or move a file or directory
 */
static int hfs_rename_impl(hfsvol *vol, char *srcpath, char *dstpath)
{
  CatDataRec src, dst;
  long srcid, dstid;
  char srcname[HFS_MAX_FLEN + 1], dstname[HFS_MAX_FLEN + 1];
  unsigned char record[HFS_CATRECMAXLEN];
  int found, isdir, moving, reclen;
  node n;

  if (v_getvol(&vol) < 0 ||
      v_resolve(vol, srcpath, &src, &srcid, srcname, 0) <= 0)
    return -1;

  isdir  = (src.cdrType == cdrDirRec);

  found = v_resolve(vol, dstpath, &dst, &dstid, dstname, 0);
  if (found < 0)
    return -1;

  if (dstid == 0)
    {
      ERROR(ENOENT, "bad destination path");
      return -1;
    }

  if (found &&
      dst.cdrType == cdrDirRec &&
      dst.u.dir.dirDirID != src.u.dir.dirDirID)
    {
      dstid = dst.u.dir.dirDirID;
      strcpy(dstname, srcname);

      found = v_catsearch(vol, dstid, dstname, 0, 0, 0);
      if (found < 0)
	return -1;
    }

  moving = (srcid != dstid);

  if (found)
    {
      char *ptr;

      ptr = strrchr(dstpath, ':');
      if (ptr == 0)
	ptr = dstpath;
      else
	++ptr;

      if (*ptr)
	strcpy(dstname, ptr);

      if (! moving && strcmp(srcname, dstname) == 0)
	return 0;  /* source and destination are the same */

      if (moving || ClassicHfsCompareNames(srcname, dstname))
	{
	  ERROR(EEXIST, "can't use destination name");
	  return -1;
	}
    }

  /* can't move anything into the root directory's parent */

  if (moving && dstid == HFS_CNID_ROOTPAR)
    {
      ERROR(EINVAL, "can't move above root directory");
      return -1;
    }

  if (moving && isdir)
    {
      long id;

      /* can't move root directory anywhere */

      if (src.u.dir.dirDirID == HFS_CNID_ROOTDIR)
	{
	  ERROR(EINVAL, "can't move root directory");
	  return -1;
	}

      /* make sure we aren't trying to move a directory inside itself */

      for (id = dstid; id != HFS_CNID_ROOTDIR; id = dst.u.dthd.thdParID)
	{
	  if (id == src.u.dir.dirDirID)
	    {
	      ERROR(EINVAL, "can't move directory inside itself");
	      return -1;
	    }

	  if (v_getdthread(vol, id, &dst, 0) <= 0)
	    return -1;
	}
    }

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  /* Open handles retain their catalog key. Do not orphan pending updates
     by changing that key underneath an active file. */
  if (!isdir) {
    hfsfile *opened;
    for (opened = vol->files; opened; opened = opened->next)
      if (opened->cat.u.fil.filFlNum == src.u.fil.filFlNum) {
        ERROR(EBUSY, "file is open; close it before renaming");
        return -1;
      }
  }

  /* A longer key can split the destination leaf (and its ancestors).
     Reserve those nodes before removing the only catalog link to the forks. */
  if (bt_space(&vol->cat, 1) < 0)
    return -1;

  /* change volume name */

  if (dstid == HFS_CNID_ROOTPAR)
    {
      if (strlen(dstname) > HFS_MAX_VLEN)
	{
	  ERROR(ENAMETOOLONG, 0);
	  return -1;
	}

      strcpy(vol->mdb.drVN, dstname);
      vol->flags |= HFS_UPDATE_MDB;
    }

  /* remove source record */

  if (ClassicHfsWriteCatalogKey(record, srcid, srcname) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }

  if (bt_delete(&vol->cat, record) < 0)
    return -1;

  /* insert destination record */

  if ((reclen = ClassicHfsWriteCatalogKey(record, dstid, dstname)) < 0) {
    ERROR(ENAMETOOLONG, "catalog name exceeds 31 bytes");
    return -1;
  }
  {
    unsigned size = ClassicHfsEncodeCatalogRecord(record + reclen, sizeof(record) - reclen, &src);
    if (!size) { ERROR(EIO, "invalid classic HFS catalog record size"); return -1; }
    reclen += size;
  }

  if (bt_insert(&vol->cat, record, reclen) < 0)
    return -1;

  /* update thread record */

  if (isdir)
    {
      if (v_getdthread(vol, src.u.dir.dirDirID, &dst, &n) <= 0)
	return -1;

      dst.u.dthd.thdParID = dstid;
      strcpy(dst.u.dthd.thdCName, dstname);

      if (v_putcatrec(&dst, &n) < 0)
	return -1;
    }
  else
    {
      found = v_getfthread(vol, src.u.fil.filFlNum, &dst, &n);
      if (found < 0)
	return -1;

      if (found)
	{
	  dst.u.dthd.thdParID = dstid;
	  strcpy(dst.u.dthd.thdCName, dstname);

	  if (v_putcatrec(&dst, &n) < 0)
	    return -1;
	}
    }

  /* update directory valences */

  if (moving)
    {
      if (v_adjvalence(vol, srcid, isdir, -1) < 0 ||
	  v_adjvalence(vol, dstid, isdir,  1) < 0)
	return -1;
    }

  return 0;
}

/* Resolve the default mount, then validate the path before starting undo. */
static int mutation_source(hfsvol **vol,char *path) {
 CatDataRec data;int found;
 if(v_getvol(vol)<0)return -1;
 found=v_resolve(*vol,path,&data,0,0,0);
 if(found==0)ERROR(ENOENT,"HFS entry not found");
 return found>0?0:-1;
}
int hfs_delete(hfsvol *vol,char *path) {
 int result;
 if(mutation_source(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_delete_impl(vol,path);return b_end_mutation(vol,result);
}
int hfs_rename(hfsvol *vol,char *source,char *destination) {
 int result;
 if(mutation_source(&vol,source)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_rename_impl(vol,source,destination);return b_end_mutation(vol,result);
}

static int mutation_target(hfsvol **vol,char *path) {
 CatDataRec data;long parent;int found;
 if(v_getvol(vol)<0)return -1;
 found=v_resolve(*vol,path,&data,&parent,0,0);
 if(found<0)return -1;
 if(!parent){ERROR(ENOENT,"HFS parent directory not found");return -1;}
 return 0;
}
int hfs_mkdir(hfsvol *vol,char *path) {
 int result;
 if(mutation_target(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_mkdir_impl(vol,path);return b_end_mutation(vol,result);
}
int hfs_create(hfsvol *vol,char *path,char *type,char *creator) {
 int result;
 if(mutation_target(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_create_impl(vol,path,type,creator);return b_end_mutation(vol,result);
}
int hfs_rmdir(hfsvol *vol,char *path) {
 int result;
 if(mutation_source(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_rmdir_impl(vol,path);return b_end_mutation(vol,result);
}
int hfs_editattrs(hfsvol *vol,char *path,char *destination,hfsdirent *entry) {
 int result;hfsdirent source,target;
 if(mutation_source(&vol,path)<0)return -1;
 if(hfs_stat(vol,path,&source)<0)return -1;
 /* This is a leaf rename, never an implicit move into an existing folder. */
 if(strcmp(path,destination)&&hfs_stat(vol,destination,&target)==0&&target.cnid!=source.cnid){ERROR(EEXIST,"Properties rename destination already exists");return -1;}
 if(b_begin_mutation(vol)<0)return -1;
 result=strcmp(path,destination)?hfs_rename_impl(vol,path,destination):0;
 if(result==0)result=hfs_setattr_impl(vol,destination,entry);
 return b_end_mutation(vol,result);
}

int hfs_setattr(hfsvol *vol,char *path,hfsdirent *entry) {
 int result;
 if(mutation_source(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 result=hfs_setattr_impl(vol,path,entry);return b_end_mutation(vol,result);
}

int hfs_getfinderinfo(hfsvol *vol,char *path,unsigned char *finder) {
 CatDataRec data;unsigned char raw[102];
 if(!finder||v_getvol(&vol)<0||v_resolve(vol,path,&data,0,0,0)<=0)return -1;
 if(data.cdrType!=cdrDirRec&&data.cdrType!=cdrFilRec){ERROR(EINVAL,"Finder information requires a file or folder");return -1;}
 ClassicHfsEncodeCatalogRecord(raw,sizeof(raw),&data);
 if(data.cdrType==cdrDirRec)memcpy(finder,raw+22,32);
 else {memcpy(finder,raw+4,16);memcpy(finder+16,raw+56,16);}return 0;
}
int hfs_fsetfinderinfo(hfsfile *file,const unsigned char *finder) {
 unsigned char raw[102];
 if(!file||!finder||file->cat.cdrType!=cdrFilRec){ERROR(EINVAL,"Missing file Finder information");return -1;}
 if(file->vol->flags&HFS_READONLY){ERROR(EROFS,0);return -1;}
 ClassicHfsEncodeCatalogRecord(raw,sizeof(raw),&file->cat);memcpy(raw+4,finder,16);memcpy(raw+56,finder+16,16);ClassicHfsDecodeCatalogRecord(raw,sizeof(raw),&file->cat);file->flags|=HFS_UPDATE_CATREC;return 0;
}
int hfs_setdirmetadata(hfsvol *vol,char *path,long expected,const unsigned char *finder,const time_t *dates,const unsigned long *backup) {
 CatDataRec data;node n;unsigned char raw[70];int result=-1;
 if(mutation_source(&vol,path)<0||b_begin_mutation(vol)<0)return -1;
 if(v_resolve(vol,path,&data,0,0,&n)<=0)goto done;
 if(data.cdrType!=cdrDirRec||data.u.dir.dirDirID!=expected){ERROR(EINVAL,"Destination folder changed during copying");goto done;}
 if(finder){ClassicHfsEncodeCatalogRecord(raw,sizeof(raw),&data);memcpy(raw+22,finder,32);ClassicHfsDecodeCatalogRecord(raw,sizeof(raw),&data);}
 if(dates){data.u.dir.dirCrDat=ClassicHfsFromUnixSeconds(dates[0]);data.u.dir.dirMdDat=ClassicHfsFromUnixSeconds(dates[1]);}
 if(backup)data.u.dir.dirBkDat=*backup;
 result=v_putcatrec(&data,&n);
 done:return b_end_mutation(vol,result);
}

int hfs_truncate(hfsfile *file,unsigned long length) {
 unsigned long *logical;int result;hfsvol *vol;
 /* Preserve the public range check before touching a caller-supplied file. */
 if(length>HFS_MAX_FORK_SIZE){ERROR(EFBIG,"classic HFS fork exceeds Apple limit");return -1;}
 logical = ClassicHfsSelectedFork(file).logical;
 if(*logical<=length)return 0;
 vol=file->vol;
 if(b_begin_mutation(vol)<0)return -1;
 result=hfs_truncate_impl(file,length);
 if(result==0&&f_trunc(file)<0)result=-1;
 if(result==0&&f_flush(file)<0)result=-1;
 return b_end_mutation(vol,result);
}

int hfs_flush(hfsvol *vol) {
 if(v_getvol(&vol)<0)return -1;
 if(vol->flags&HFS_READONLY)return hfs_flush_raw(vol);
 if(b_begin_mutation(vol)<0)return -1;
 return b_end_mutation(vol,0);
}
