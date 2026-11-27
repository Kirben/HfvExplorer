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
#include "../ClassicHfsCatalogLeaf.h"
#ifdef WIN32
# include "../../ClassicHfsRecovery.h"
#endif
#include "../ClassicHfsTime.h"
#include "../ClassicHfsBytes.h"
# include "block.h"
#include "../ClassicHfsVolumeBitmap.h"
# include "file.h"
#include "../ClassicHfsSystemFiles.h"
#include "../ClassicHfsFormatTree.h"
# include "btree.h"
#include "../ClassicHfsTreeMount.h"
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
  return ClassicHfsLocateVolume(&vol)==0 && ClassicHfsLoadVolumeHeader(&vol)==0 && vol.mdb.drSigWord==HFS_SIGWORD;
}

/*
 * NAME:	hfs->mount()
 * DESCRIPTION:	open an HFS volume; return volume descriptor or 0 (error)
 */
int ClassicHfsCurrentVolume(hfsvol **volume)
{
    if (!*volume)
        *volume = hfs_curvol;
    if (*volume)
        return 0;
    ERROR(EINVAL, "no volume is current");
    return -1;
}

// Windows file hooks establish sharing when acquiring the descriptor. On
// hosts with POSIX device locking, keep the advisory lock until close.
int ClassicHfsOpenVolumeDevice(char *path, int flags)
{
#ifdef __EMX__
    flags |= O_BINARY;
#endif
    int fd = open(path, flags);
#if !defined(WIN32) && !defined(NODEVLOCKS)
    if (fd >= 0) {
        struct flock lock = {0};
        lock.l_type = (flags & (O_WRONLY | O_RDWR)) ? F_WRLCK : F_RDLCK;
        lock.l_whence = SEEK_SET;
        if (fcntl(fd, F_SETLK, &lock) < 0) {
            int error = errno;
            close(fd);
            ERROR(error, "unable to obtain lock for device");
            return -2;
        }
    }
#endif
    return fd;
}

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
	  vol->fd = ClassicHfsOpenVolumeDevice(path, O_RDWR);
	}

      if (supplied_fd < 0 && (! (flags & (O_WRONLY | O_RDWR)) ||
	  (vol->fd < 0 &&
	   (errno == EROFS || errno == EACCES || errno == EAGAIN) &&
	   (flags & O_RDWR))))
	{
	  vol->flags |= HFS_READONLY;
	  vol->fd = ClassicHfsOpenVolumeDevice(path, O_RDONLY);
	}

      if (vol->fd < 0)
	{
	  if (vol->fd != -2)
	    ERROR(errno, errno==EBUSY ? "image is in use by another storage session" : "error opening device");

	  ClassicHfsReleaseVolume(vol);

	  return 0;
	}

#ifdef WIN32
      recovered = hfv_hfs_recover_image(vol->fd, !(vol->flags & HFS_READONLY));
      if (recovered < 0)
        {
          close(vol->fd);
          ClassicHfsReleaseVolume(vol);
          ERROR(EIO,"Classic HFS recovery could not complete. Keep the image and reopen with exclusive writable access.");
          return 0;
        }
#endif

      /* find out what kind of media this is and read the MDB */

      if (ClassicHfsLocateVolume(vol) < 0 ||
	  ClassicHfsLoadVolumeHeader(vol) < 0)
	{
	  close(vol->fd);
	  ClassicHfsReleaseVolume(vol);

	  return 0;
	}

      /* verify this is an HFS volume */

      if (vol->mdb.drSigWord != HFS_SIGWORD ||
          (session && (vol->mdb.drVCSize == 0x482b || vol->mdb.drVCSize == 0x4858)))
	{
	  close(vol->fd);
	  ClassicHfsReleaseVolume(vol);

	  ERROR(EINVAL, "not a Macintosh HFS volume");
	  return 0;
	}

      /* do minimal consistency checks */

      if (!vol->mdb.drAlBlkSiz || vol->mdb.drAlBlkSiz % HFS_BLOCKSZ != 0)
	{
	  close(vol->fd);
	  ClassicHfsReleaseVolume(vol);

	  ERROR(EINVAL, "bad volume allocation block size");
	  return 0;
	}

      if (vol->vlen == 0) {
          uint32_t sectors;
          if (!ClassicHfsVolumeSectors(vol->mdb.drAlBlSt, vol->mdb.drNmAlBlks,
                                       vol->mdb.drAlBlkSiz >> HFS_BLOCKSZ_BITS, &sectors)) {
              close(vol->fd);
              ClassicHfsReleaseVolume(vol);
              ERROR(EINVAL, "invalid classic HFS volume length");
              return 0;
          }
          vol->vlen = sectors;
      }

      /* initialize volume block cache (OK to fail) */

      ClassicHfsEnableVolumeCache(vol);

      /* read the volume bitmap and extents/catalog B*-tree headers */

      if (ClassicHfsLoadVolumeBitmap(vol) < 0 ||
	  ClassicHfsLoadTreeHeader(&vol->ext) < 0 ||
	  ClassicHfsLoadTreeHeader(&vol->cat) < 0)
	{
	  close(vol->fd);
	  ClassicHfsReleaseVolume(vol);
	  return 0;
	}

      if (! (vol->mdb.drAtrb & HFS_ATRB_UMOUNTED))
	{
          if (session && !recovered)
            vol->flags |= HFS_READONLY;
	  /* volume was not cleanly unmounted; scavenge free-space */

	  if (ClassicHfsRecoverAllocationBitmap(vol) < 0)
	    {
	      close(vol->fd);
	      ClassicHfsReleaseVolume(vol);
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
  if (ClassicHfsBeginTransaction(vol)<0) return -1;
  vol->mdb.drAtrb &= ~HFS_ATRB_UMOUNTED;
  vol->flags |= HFS_UPDATE_MDB;
  return ClassicHfsEndTransaction(vol,0);
}

/* Publish a clean marker while the caller still owns the durable undo log. */
int hfs_mark_session_clean(hfsvol *vol) {
  if (hfs_flush(vol)<0 || ClassicHfsBeginTransaction(vol)<0) return -1;
  return ClassicHfsEndTransaction(vol,ClassicHfsFlushVolumeMetadata(vol,1));
}

/*
 * NAME:	hfs->flush()
 * DESCRIPTION:	flush all pending changes to an HFS volume
 */
int hfs_flush_raw(hfsvol *vol)
{
  hfsfile *file;

  if (ClassicHfsCurrentVolume(&vol) < 0)
    return -1;

  if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes are disabled after rollback failure");return -1;}

  for (file = vol->files; file; file = file->next)
    {
      if (ClassicHfsFlushOpenFile(file) < 0)
	return -1;
    }

  if (ClassicHfsFlushVolumeMetadata(vol, 0) < 0 ||
      ClassicHfsFlushVolumeCache(vol) < 0)
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
  if (ClassicHfsCurrentVolume(&vol) < 0) return -1;
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
    } else if(ClassicHfsBeginTransaction(vol)<0) {
      result=-1;saved_errno=errno;saved_error=hfs_error;
    } else {
      int marked=ClassicHfsFlushVolumeMetadata(vol,1);
      if(ClassicHfsEndTransaction(vol,marked)<0) {
        result=-1;saved_errno=errno;saved_error=hfs_error;
      }
    }
  }
  /* A failed finalization must not be retried outside undo by ClassicHfsCloseVolumeCache.
     Successful rollback has already restored and flushed the old sectors. */
  if(result!=0){FREE(vol->cache);vol->cache=0;}
  /* Always release the block cache, even after a metadata flush error. */
  if (ClassicHfsCloseVolumeCache(vol) < 0 && result == 0) {
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
  ClassicHfsReleaseVolume(vol);
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

int hfs_flush(hfsvol *vol) {
 if(ClassicHfsCurrentVolume(&vol)<0)return -1;
 if(vol->flags&HFS_READONLY)return hfs_flush_raw(vol);
 if(ClassicHfsBeginTransaction(vol)<0)return -1;
 return ClassicHfsEndTransaction(vol,0);
}
