/*
 * HFVExplorer
 * Copyright (C) 1997-1998 by Anygraaf Oy
 * Author: Lauri Pesonen, email: lpesonen@clinet.fi or lauri.pesonen@anygraaf.fi
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

#include "ClassicImagePath.h"
#include <windows.h>
#include <sys/types.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <share.h>
#include <stdarg.h>

#define _FHOOK_INTERNAL_
#include "filehook.h"

/* 
	Hook:
		hfs\ClassicHfsMount.c
		hfs\ClassicHfsVolumeIO.c

	Do not hook:
		hfs\copyin.c
		hfs\copyout.c
		hfs\binhex.c

	Missing:
		function "stat".
		Just make sure to give file name which does not exist.
*/

// Native sharing is enforced by file identity, including hard links and path aliases.
// Read handles share with readers; writers deny every other write-capable open.
// A read-only libhfs session also denies a later writer while its caches are alive.
static int encoded_image_path(const char* path)
{
 return path&&strncmp(path,HFV_CLASSIC_IMAGE_PREFIX,sizeof(HFV_CLASSIC_IMAGE_PREFIX)-1)==0;
}

static wchar_t* wide_image_path(const char* path)
{
 int length;
 wchar_t* result;
 path+=sizeof(HFV_CLASSIC_IMAGE_PREFIX)-1;
 length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,0,0);
 if(length<=1){errno=EINVAL;return 0;}
 result=(wchar_t*)malloc((size_t)length*sizeof(*result));
 if(!result){errno=ENOMEM;return 0;}
 if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,result,length)){
  free(result);errno=EINVAL;return 0;
 }
 return result;
}

int fhook_stat_image(const char* path,struct _stat64* st)
{
 int result;
 wchar_t* wide;
 if(!encoded_image_path(path))return _stat64(path,st);
 wide=wide_image_path(path);
 if(!wide)return -1;
 result=_wstat64(wide,st);
 free(wide);
 return result;
}

int fhook_open_image(const char* path,int flags,...) {
 int fd=-1,mode=0;errno_t result;
 if(flags&_O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
 // Disk sectors must bypass the CRT text-mode EOF and newline handling.
 if(encoded_image_path(path)){
  wchar_t* wide=wide_image_path(path);
  if(!wide)return -1;
  result=_wsopen_s(&fd,wide,flags|_O_NOINHERIT|_O_BINARY,_SH_DENYWR,mode);
  free(wide);
 }else result=_sopen_s(&fd,path,flags|_O_NOINHERIT|_O_BINARY,_SH_DENYWR,mode);
 if(result){unsigned long error=0;_get_doserrno(&error);
  errno=(error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION)?EBUSY:result;
  return -1;}
 return fd;
}

ftype_stat  fhook_stat  = fhook_stat_image;
ftype_open  fhook_open  = fhook_open_image;
ftype_close fhook_close = close;
ftype_sync fhook_sync = _commit;
ftype_read  fhook_read  = read;
ftype_write fhook_write = write;
ftype_lseek fhook_lseek = _lseeki64;

/* CRT st_ino is not a unique Windows file ID. In particular, junction paths
 * can also report a different st_dev than the open descriptor. */
static ftype_identity identity_hook=0;
void fhook_init_identity(ftype_identity hook){identity_hook=hook;}
int fhook_same_file(int fd,const char* path){
 HANDLE a,b;BY_HANDLE_FILE_INFORMATION x,y;int result=0;
 if(identity_hook){int answer=identity_hook(fd,path);if(answer>=0)return answer;}
 if(fd<0||fd>=0x10000000)return 0; /* Never pass virtual descriptors to the CRT. */
 a=(HANDLE)_get_osfhandle(fd);if(a==INVALID_HANDLE_VALUE)return 0;
 if(encoded_image_path(path)){
  wchar_t* wide=wide_image_path(path);
  if(!wide)return 0;
  b=CreateFileW(wide,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
  free(wide);
 }else b=CreateFileA(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
 if(b==INVALID_HANDLE_VALUE)return 0;
 if(GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y))
  result=x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
 CloseHandle(b);return result;
}
static ftype_stat  fake_stat = 0;
static ftype_open  fake_open = 0;
static ftype_close fake_close = 0;
static ftype_sync fake_sync = 0;
static int missing_sync(int fd) { (void)fd; errno=EIO; return -1; }
void fhook_init_sync(ftype_sync sync) {fake_sync=sync;}
static ftype_read  fake_read = 0;
static ftype_write fake_write = 0;
static ftype_lseek fake_lseek = 0;

static void fhook_init_fsystem( void )
{
}

void fhook_set_real_funcs( void )
{
	fhook_stat =  fhook_stat_image;
	fhook_open =  fhook_open_image;
	fhook_close = close;
	fhook_sync = _commit;
	fhook_read =  read;
	fhook_write = write;
	fhook_lseek = _lseeki64;
}

void fhook_set_fake_funcs( void )
{
	fhook_stat =  fake_stat;
	fhook_open =  fake_open;
	fhook_close = fake_close;
	fhook_sync = fake_sync?fake_sync:missing_sync;
	fhook_read =  fake_read;
	fhook_write = fake_write;
	fhook_lseek = fake_lseek;
	fhook_init_fsystem();
}

void fhook_init_fake_funcs(
	ftype_stat  f_stat,
	ftype_open  f_open,
	ftype_close f_close,
	ftype_read  f_read,
	ftype_write f_write,
	ftype_lseek f_lseek
)
{
	fake_stat =  f_stat;
	fake_open =  f_open;
	fake_close = f_close;
	fake_read =  f_read;
	fake_write = f_write;
	fake_lseek = f_lseek;
}
