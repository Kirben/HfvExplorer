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

#include <errno.h>
#include <string.h>
#include "ClassicHfsEngine.h"
#include "ClassicHfsCatalog.h"
#include "ClassicHfsEntry.h"
#include "ClassicHfsVolumeIO.h"

/*
 * NAME:	hfs->vstat()
 * DESCRIPTION:	return volume statistics
 */
int hfs_vstat(hfsvol *vol, hfsvolent *ent)
{
  if (ClassicHfsCurrentVolume(&vol) < 0)
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

  if (ClassicHfsCurrentVolume(&vol) < 0)
    return -1;
  int found = ClassicHfsResolvePath(vol, path, &data, &parid, name, NULL);
  if (!found)
    ERROR(ENOENT, "HFS entry not found");
  if (found <= 0)
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

  if (ClassicHfsCurrentVolume(&vol) < 0 ||
      ClassicHfsResolvePath(vol, path, &data, 0, 0, &n) <= 0)
    return -1;

  if (vol->flags & HFS_READONLY)
    {
      ERROR(EROFS, 0);
      return -1;
    }

  ClassicHfsApplyEntry(&data, ent);

  if (ClassicHfsStoreCatalogEntry(&data, &n) < 0)
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

/* Resolve the default mount, then validate the path before starting undo. */
static int mutation_source(hfsvol **vol,char *path) {
 CatDataRec data;int found;
 if(ClassicHfsCurrentVolume(vol)<0)return -1;
 found=ClassicHfsResolvePath(*vol,path,&data,0,0,0);
 if(found==0)ERROR(ENOENT,"HFS entry not found");
 return found>0?0:-1;
}
int hfs_rename(hfsvol *vol,char *source,char *destination) {
 int result;
 if(mutation_source(&vol,source)<0||ClassicHfsBeginTransaction(vol)<0)return -1;
 result=ClassicHfsRenameCatalogPath(vol,source,destination);return ClassicHfsEndTransaction(vol,result);
}

int hfs_editattrs(hfsvol *vol,char *path,char *destination,hfsdirent *entry) {
 int result;
 hfsdirent source;
 if(ClassicHfsCurrentVolume(&vol)<0 || hfs_stat(vol,path,&source)<0)return -1;
 if(strcmp(path,destination)) {
   CatDataRec target;
   int found=ClassicHfsResolvePath(vol,destination,&target,NULL,NULL,NULL);
   if(found<0)return -1;
   if(found && (target.cdrType==cdrDirRec ? target.u.dir.dirDirID : target.u.fil.filFlNum)!=source.cnid) {
     ERROR(EEXIST,"Properties rename destination already exists");
     return -1;
   }
 }
 if(ClassicHfsBeginTransaction(vol)<0)return -1;
 result=strcmp(path,destination)?ClassicHfsRenameCatalogPath(vol,path,destination):0;
 if(result==0)result=hfs_setattr_impl(vol,destination,entry);
 return ClassicHfsEndTransaction(vol,result);
}

int hfs_setattr(hfsvol *vol,char *path,hfsdirent *entry) {
 int result;
 if(mutation_source(&vol,path)<0||ClassicHfsBeginTransaction(vol)<0)return -1;
 result=hfs_setattr_impl(vol,path,entry);return ClassicHfsEndTransaction(vol,result);
}

int hfs_getfinderinfo(hfsvol *vol,char *path,unsigned char *finder) {
 CatDataRec data;unsigned char raw[102];
 if(!finder||ClassicHfsCurrentVolume(&vol)<0||ClassicHfsResolvePath(vol,path,&data,0,0,0)<=0)return -1;
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
 if(mutation_source(&vol,path)<0||ClassicHfsBeginTransaction(vol)<0)return -1;
 if(ClassicHfsResolvePath(vol,path,&data,0,0,&n)<=0)goto done;
 if(data.cdrType!=cdrDirRec||data.u.dir.dirDirID!=expected){ERROR(EINVAL,"Destination folder changed during copying");goto done;}
 if(finder){ClassicHfsEncodeCatalogRecord(raw,sizeof(raw),&data);memcpy(raw+22,finder,32);ClassicHfsDecodeCatalogRecord(raw,sizeof(raw),&data);}
 if(dates){data.u.dir.dirCrDat=ClassicHfsFromUnixSeconds(dates[0]);data.u.dir.dirMdDat=ClassicHfsFromUnixSeconds(dates[1]);}
 if(backup)data.u.dir.dirBkDat=*backup;
 result=ClassicHfsStoreCatalogEntry(&data,&n);
 done:return ClassicHfsEndTransaction(vol,result);
}
