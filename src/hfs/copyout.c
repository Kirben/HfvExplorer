#include "EngineContext.h"
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

#ifdef WIN32
# include <windows.h>
# include <io.h>
# include <stdio.h>
# include <direct.h>
#define S_ISDIR(x) ( ((x) & _S_IFDIR) != 0 )
#define STDOUT_FILENO _fileno(stdout)
#else
# include <unistd.h>
#endif

#ifdef __EMX__
# include <stdlib.h>
# include <sys/types.h>
#endif
# include <sys/stat.h>
# include <fcntl.h>
# include <string.h>

# include "libhfs\hfs.h"
# include "libhfs\data.h"
# include "copyout.h"
# include "binhex.h"
# include "crc.h"
#ifdef __EMX__
# include "stdio.h"
# include "io.h"
#endif

void mac_to_pc_charset( unsigned char *s );

extern HFV_THREAD_LOCAL int crlf_translation;

#ifdef WIN32
/* Stage output next to its destination. Never truncate the previous copy. */
static HFV_THREAD_LOCAL char copyout_temporary[_MAX_PATH], copyout_destination[_MAX_PATH];
static HFV_THREAD_LOCAL int copyout_replace;
#ifdef HFV_TESTING
static HFV_THREAD_LOCAL int copyout_fault;
void copyout_test_fault(int stage) { copyout_fault=stage; }
#else
#define copyout_fault 0
#endif
static int stage_output(const char* path, int replace) {
 char folder[_MAX_PATH],*leaf;DWORD length;
 HANDLE handle;int fd;
 length=GetFullPathNameA(path,sizeof(copyout_destination),copyout_destination,&leaf);
 if(!length||length>=sizeof(copyout_destination)||!leaf)return -1;
 strcpy(folder,copyout_destination);folder[leaf-copyout_destination]=0;
 if(!GetTempFileNameA(folder,"HFV",0,copyout_temporary))return -1;
 handle=CreateFileA(copyout_temporary,GENERIC_WRITE,0,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
 if(handle==INVALID_HANDLE_VALUE){DeleteFileA(copyout_temporary);*copyout_temporary=0;return -1;}
 fd=_open_osfhandle((intptr_t)handle,O_BINARY|O_WRONLY);
 if(fd<0){CloseHandle(handle);DeleteFileA(copyout_temporary);*copyout_temporary=0;return -1;}
 copyout_replace=replace;
 return fd;
}
static int checked_output_write(int fd,const void* data,unsigned count) {
 if(copyout_fault==1){return -1;}
 return _write(fd,data,count);
}
#define write checked_output_write
#endif

static HFV_THREAD_LOCAL int copyout_opened=0;
int copyout_destination_opened(void){return copyout_opened;}
static HFV_THREAD_LOCAL int copyout_skipped=0;
int copyout_was_skipped(void){return copyout_skipped;}
static HFV_THREAD_LOCAL copyout_overwrite_fn copyout_overwrite=0;
static HFV_THREAD_LOCAL void* copyout_overwrite_context=0;
void copyout_set_overwrite(copyout_overwrite_fn observer,void* context) {copyout_overwrite=observer;copyout_overwrite_context=context;}
int copyout_hfs_overwrite(const char *name,int exists) {
 int decision=1;char display[1024];
 copyout_skipped=0;
 if(exists&&copyout_overwrite) {
  if(strlen(name)>=sizeof(display))return 0;
  strcpy(display,name);mac_to_pc_charset((unsigned char*)display);
  decision=copyout_overwrite(display,copyout_overwrite_context);
 }
 if(decision==2)copyout_skipped=1;
 return decision;
}
static HFV_THREAD_LOCAL copyout_progress_fn copyout_observer = 0;
static HFV_THREAD_LOCAL void *copyout_context = 0;
static HFV_THREAD_LOCAL char copyout_source[_MAX_PATH];
static HFV_THREAD_LOCAL unsigned copyout_forks=3;
static HFV_THREAD_LOCAL unsigned long long copyout_expected=0;
void copyout_set_progress(copyout_progress_fn observer,void *context) {
 copyout_observer=observer;copyout_context=context;
}
static long copyout_read(hfsfile *file,void *buffer,unsigned long length) {
 long result=hfs_read(file,buffer,length);
 if(result>0 && copyout_observer)copyout_observer(copyout_source,(unsigned long)result,copyout_expected,0,copyout_context);
 return result;
}
#define hfs_read copyout_read
HFV_THREAD_LOCAL char *cpo_error = "no error";

static HFV_THREAD_LOCAL char m_last_opened_file[_MAX_PATH];

void copyout_set_last_open_file( const char *path )
{
	*m_last_opened_file = 0;
	if(path) strncpy( m_last_opened_file, path, sizeof(m_last_opened_file) );
}

void copyout_set_last_open_file2( const char *dir, const char *path )
{
	int len;
	char fullpath[_MAX_PATH];

	strcpy( fullpath, dir );
	len = strlen( fullpath );
	if(len > 0 && fullpath[len-1] != '\\') strcat( fullpath, "\\" );
	strcat( fullpath, path );
	copyout_set_last_open_file( fullpath );
}

void copyout_get_last_open_file( char *path, int maxpath )
{
	strncpy( path, m_last_opened_file, maxpath );
}

# define MACB_BLOCKSZ	128

/* Copy Routines =========================================================== */

#ifdef WIN32
/* Windows-to-Windows copies use the same publication rules as HFS exports. */
int copyout_host_copy(const char *source,const char *destination) {
 HANDLE input,existing;BY_HANDLE_FILE_INFORMATION from,to;
 DWORD attributes,error,copiedAttributes;int fd=-1,result=-1,decision=1;char absolute[_MAX_PATH];
 unsigned long long length;
 copyout_skipped=0;copyout_opened=0;
 input=CreateFileA(source,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
 if(input==INVALID_HANDLE_VALUE){cpo_error="Cannot open Windows source";return 1;}
 if(!GetFileInformationByHandle(input,&from)||(from.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))goto done;
 length=((unsigned long long)from.nFileSizeHigh<<32)|from.nFileSizeLow;
 existing=CreateFileA(destination,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
 if(existing!=INVALID_HANDLE_VALUE) {
  int same=GetFileInformationByHandle(existing,&to)&&from.dwVolumeSerialNumber==to.dwVolumeSerialNumber&&
   from.nFileIndexHigh==to.nFileIndexHigh&&from.nFileIndexLow==to.nFileIndexLow;
  CloseHandle(existing);
  if(same){cpo_error="Source and destination are the same Windows file";goto done;}
 }
 attributes=GetFileAttributesA(destination);error=GetLastError();
 if(attributes==INVALID_FILE_ATTRIBUTES&&error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)goto done;
 if(copyout_observer)copyout_observer(source,0,length,0,copyout_context);
 if(attributes!=INVALID_FILE_ATTRIBUTES&&copyout_overwrite) {
  DWORD n=GetFullPathNameA(destination,sizeof(absolute),absolute,0);
  if(!n||n>=sizeof(absolute))goto done;
  decision=copyout_overwrite(absolute,copyout_overwrite_context);
 }
 if(decision==2){copyout_skipped=1;result=0;goto done;}
 if(decision<=0){cpo_error="Copy cancelled";goto done;}
 fd=stage_output(destination,attributes!=INVALID_FILE_ATTRIBUTES);
 if(fd<0){cpo_error="Cannot stage Windows output";goto done;}
 cpo_error="Windows copy read/write failed";
 {int closing=fd;fd=-1;if(close(closing)<0)goto done;}
 /* CopyFile preserves alternate data streams, attributes and timestamps. */
 if(copyout_fault==1||!CopyFileA(source,copyout_temporary,FALSE))goto done;
 copiedAttributes=GetFileAttributesA(copyout_temporary);
 if(copiedAttributes==INVALID_FILE_ATTRIBUTES||!SetFileAttributesA(copyout_temporary,copiedAttributes&~FILE_ATTRIBUTE_READONLY))goto done;
 {HANDLE output=CreateFileA(copyout_temporary,GENERIC_WRITE,0,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
  if(output==INVALID_HANDLE_VALUE)goto done;
  fd=_open_osfhandle((intptr_t)output,O_BINARY|O_WRONLY);
  if(fd<0){CloseHandle(output);goto done;}
 }
 if(copyout_fault==2||_commit(fd)<0)goto done;
 {int closing=fd;fd=-1;if(close(closing)<0)goto done;}
 if(!SetFileAttributesA(copyout_temporary,copiedAttributes))goto done;
 if(copyout_fault==3||!MoveFileExA(copyout_temporary,copyout_destination,
    MOVEFILE_WRITE_THROUGH|(copyout_replace?MOVEFILE_REPLACE_EXISTING:0)))goto done;
 *copyout_temporary=0;result=0;
 if(copyout_observer)copyout_observer(source,0,length,1,copyout_context);
 done:
 if(fd>=0)close(fd);
 if(*copyout_temporary){SetFileAttributesA(copyout_temporary,FILE_ATTRIBUTE_NORMAL);DeleteFileA(copyout_temporary);*copyout_temporary=0;}
 CloseHandle(input);return result==0?0:1;
}
#endif

/*
 * NAME:	do_macb()
 * DESCRIPTION:	perform copy using MacBinary I/II translation
 */
static
int do_macb(hfsfile *ifile, int ofile, int legacy)
{
  hfsdirent ent;
  unsigned char buf[HFS_BLOCKSZ * 4];
  long bytes;
  unsigned long total;

  if (hfs_fstat(ifile, &ent) < 0)
    {
      cpo_error = hfs_error;
      return -1;
    }

  cpo_error = "error writing data";

  memset(buf, 0, MACB_BLOCKSZ);

  buf[1] = strlen(ent.name);
  strcpy((char *) &buf[2], ent.name);

  memcpy(&buf[65], ent.u.file.type,    4);
  memcpy(&buf[69], ent.u.file.creator, 4);

  buf[73] = ent.fdflags >> 8;

  d_putl(&buf[83], ent.u.file.dsize);
  d_putl(&buf[87], ent.u.file.rsize);

  d_putl(&buf[91], d_tomtime(ent.crdate));
  d_putl(&buf[95], d_tomtime(ent.mddate));

  d_putw(buf + 75, ent.fdlocation.v);
  d_putw(buf + 77, ent.fdlocation.h);
  buf[81] = (ent.flags & HFS_ISLOCKED) ? 1 : 0;
  if (!legacy) {
    buf[101] = ent.fdflags & 0xff;
    buf[122] = buf[123] = 129;
    d_putw(&buf[124], crc_macb(buf, 124, 0x0000));
  }

  if (write(ofile, buf, MACB_BLOCKSZ) < MACB_BLOCKSZ)
    return -1;

  /* data fork */

  total = 0;
  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      if (write(ofile, buf, bytes) < bytes)
	return -1;

      total += bytes;
    }

# ifdef DEBUG
  if (total != ent.u.file.dsize)
    {
      cpo_error = "inconsistent data fork length";
      return -1;
    }
# endif

  bytes = total % MACB_BLOCKSZ;
  if (bytes)
    {
      memset(buf, 0, MACB_BLOCKSZ);
      if (write(ofile, buf, MACB_BLOCKSZ - bytes) < MACB_BLOCKSZ - bytes)
	return -1;
    }

  /* resource fork */

  if (hfs_setfork(ifile, 1) < 0)
    {
      cpo_error = hfs_error;
      return -1;
    }

  total = 0;
  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      if (write(ofile, buf, bytes) < bytes)
	return -1;

      total += bytes;
    }

# ifdef DEBUG
  if (total != ent.u.file.rsize)
    {
      cpo_error = "inconsistent resource fork length";
      return -1;
    }
# endif

  bytes = total % MACB_BLOCKSZ;
  if (bytes)
    {
      memset(buf, 0, MACB_BLOCKSZ);
      if (write(ofile, buf, MACB_BLOCKSZ - bytes) < MACB_BLOCKSZ - bytes)
	return -1;
    }

  return 0;
}

/*
 * NAME:	binhx()
 * DESCRIPTION:	auxiliary BinHex routine
 */
static
int binhx(hfsfile *ifile)
{
  hfsdirent ent;
  unsigned char byte, word[2], lword[4], buf[HFS_BLOCKSZ * 4];
  long bytes;
  unsigned long total;

  if (hfs_fstat(ifile, &ent) < 0)
    {
      cpo_error = hfs_error;
      return -1;
    }

  cpo_error = "error writing data";

  byte = strlen(ent.name);
  if (bh_insert(&byte, 1) < 0 ||
      bh_insert(ent.name, byte + 1) < 0 ||
      bh_insert(ent.u.file.type, 4) < 0 ||
      bh_insert(ent.u.file.creator, 4) < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  d_putw(word, ent.fdflags);
  if (bh_insert(word, 2) < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  d_putl(lword, ent.u.file.dsize);
  if (bh_insert(lword, 4) < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  d_putl(lword, ent.u.file.rsize);
  if (bh_insert(lword, 4) < 0 ||
      bh_insertcrc() < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  /* data fork */

  total = 0;
  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      if (bh_insert(buf, bytes) < 0)
	{
	  cpo_error = bh_error;
	  return -1;
	}

      total += bytes;
    }

# ifdef DEBUG
  if (total != ent.u.file.dsize)
    {
      cpo_error = "inconsistent data fork length";
      return -1;
    }
# endif

  if (bh_insertcrc() < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  /* resource fork */

  if (hfs_setfork(ifile, 1) < 0)
    {
      cpo_error = hfs_error;
      return -1;
    }

  total = 0;
  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      if (bh_insert(buf, bytes) < 0)
	{
	  cpo_error = bh_error;
	  return -1;
	}

      total += bytes;
    }

# ifdef DEBUG
  if (total != ent.u.file.rsize)
    {
      cpo_error = "inconsistent resource fork length";
      return -1;
    }
# endif

  if (bh_insertcrc() < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  return 0;
}

/*
 * NAME:	do_binh()
 * DESCRIPTION:	perform copy using BinHex translation
 */
static
int do_binh(hfsfile *ifile, int ofile)
{
  int result;

  if (bh_start(ofile) < 0)
    {
      cpo_error = bh_error;
      return -1;
    }

  result = binhx(ifile);

  if (bh_end() < 0 && result == 0)
    {
      cpo_error = bh_error;
      result = -1;
    }

  return result;
}

/*
 * NAME:	do_text()
 * DESCRIPTION:	perform copy using text translation
 */
static
int do_text(hfsfile *ifile, int ofile)
{
  unsigned char buf[HFS_BLOCKSZ * 4], *ptr;
  long bytes;

#ifdef __EMX__
  /* Set text mode */
  setmode(ofile, O_TEXT);
#endif
  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      for (ptr = buf; ptr < buf + bytes; ++ptr)
	{
	  if (*ptr == '\r')
	    *ptr = '\n';
	}

      if (write(ofile, buf, bytes) < bytes)
	{
	  cpo_error = "error writing data";
	  return -1;
	}
    }

  return 0;
}

/*
 * NAME:	do_raw()
 * DESCRIPTION:	perform copy using no translation
 */
static
int do_raw(hfsfile *ifile, int ofile)
{
  unsigned char buf[HFS_BLOCKSZ * 4];
  long bytes;

  while (1)
    {
      bytes = hfs_read(ifile, buf, sizeof(buf));
      if (bytes < 0)
	{
	  cpo_error = hfs_error;
	  return -1;
	}
      else if (bytes == 0)
	break;

      if (write(ofile, buf, bytes) < bytes)
	{
	  cpo_error = "error writing data";
	  return -1;
	}
    }

  return 0;
}

/* Utility Routines ======================================================== */

/*
 * NAME:	opensrc()
 * DESCRIPTION:	open the source file; set hint for destination filename
 */
static
hfsfile *opensrc(hfsvol *vol, char *srcname, char **dsthint, char *ext)
{
  hfsfile *file;
  hfsdirent ent;
  static HFV_THREAD_LOCAL char name[36], *ptr;

  file = hfs_open(vol, srcname);
  if (file == 0)
    {
      cpo_error = hfs_error;
      return 0;
    }

  if (hfs_fstat(file, &ent) < 0)
    {
      hfs_close(file);
      cpo_error = hfs_error;
      return 0;
    }

  strncpy(copyout_source,srcname,sizeof(copyout_source)-1);copyout_source[sizeof(copyout_source)-1]=0;
  copyout_expected=((copyout_forks&1)?(unsigned long long)ent.u.file.dsize:0)+((copyout_forks&2)?(unsigned long long)ent.u.file.rsize:0);
  if(copyout_observer)copyout_observer(copyout_source,0,copyout_expected,0,copyout_context);
  strcpy(name, ent.name);

	if(strcmp(name,"Icon\x00D") == 0) {
		name[4] = '~';
	} else {
		mac_to_pc_charset( (unsigned char *)name );
		for (ptr = name; *ptr; ++ptr) {
			switch (*ptr) {
				case '/':
				case '*':
				case '?':
				case '\"':
				case '<':
				case '>':
				case '|':
					*ptr = '-';
					break;

				/*
				case ' ':
					*ptr = '_';
					break;
				*/
				}
		}
	}

  if (ext)
    strcat(name, ext);

  *dsthint = name;

  return file;
}

/*
 * NAME:	opendst()
 * DESCRIPTION:	open the destination file
 */
static
int opendst(char *dstname, char *hint)
{
#ifdef WIN32
 char resolved[_MAX_PATH],absolute[_MAX_PATH];DWORD attributes,error;int existing,decision=1;
 attributes=GetFileAttributesA(dstname);
 if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)){
  size_t length=strlen(dstname);int separator=length&&dstname[length-1]!='\\'&&dstname[length-1]!='/';
  if(length+separator+strlen(hint)>=sizeof(resolved)){cpo_error="Destination path is too long";return -1;}
  strcpy(resolved,dstname);if(separator)strcat(resolved,"\\");strcat(resolved,hint);dstname=resolved;
 }
 copyout_set_last_open_file(dstname);
 attributes=GetFileAttributesA(dstname);error=GetLastError();existing=attributes!=INVALID_FILE_ATTRIBUTES;
 if(!existing&&error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)decision=-1;
 else if(existing&&copyout_overwrite){
  DWORD length=GetFullPathNameA(dstname,sizeof(absolute),absolute,0);
  if(!length||length>=sizeof(absolute))decision=-1;else decision=copyout_overwrite(absolute,copyout_overwrite_context);
 }
 if(decision==2)return -2;
 if(decision<=0){cpo_error=decision==0?"Copy cancelled":"Could not check the destination";return -1;}
 return stage_output(dstname,existing);
#else

  struct stat sbuf;
#ifdef WIN32
	char curdirbuffer[_MAX_PATH];
#endif
#ifdef __EMX__
  int fd;
  char *curdir, buf[256];
#else
  int fd, dirfd;
#endif

#ifndef WIN32
#ifdef __EMX__
  /* Get absolute pathname of dstname to handle 'A:' correctly. */
  if(strcmp(dstname, "-"))
    { 
      if(_abspath(buf, dstname, sizeof(buf)) < 0) 
        {
          cpo_error = "error making absolute pathname";
          return -1;
        }
      dstname = buf;
    }
#endif
#endif

  if (stat(dstname, &sbuf) >= 0 &&
      S_ISDIR(sbuf.st_mode))
    {
#ifdef WIN32
      if((curdir=_getcwd(curdirbuffer, _MAX_PATH)) == NULL)
#elif __EMX__
  if (strcmp(dstname, "-") == 0) {
    _fsetmode(stdout, "b");
    fd = dup(STDOUT_FILENO);
  }
  else
    fd = open(dstname, O_BINARY | O_WRONLY | O_CREAT | O_TRUNC, 0666);
#else
      dirfd = open(".", O_RDONLY);
      if (dirfd < 0)
#endif
        {
	  cpo_error = "error getting current directory";
	  return -1;
	}

#ifdef WIN32
			copyout_set_last_open_file2( dstname, hint );
      if (_chdir(dstname) < 0)
#else
      if (_chdir2(dstname) < 0)
#endif
	{
	  cpo_error = "error setting current directory";
#ifndef __EMX__
	  close(dirfd);
#endif
	  return -1;
	}

      dstname = hint;
    }

#ifdef WIN32
  {
   int decision=1;DWORD attributes=GetFileAttributesA(dstname),error=GetLastError();
   int existing=attributes!=INVALID_FILE_ATTRIBUTES;char absolute[_MAX_PATH];
   if(!existing&&error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)decision=-1;
   else if(existing&&copyout_overwrite) {
    DWORD length=GetFullPathNameA(dstname,sizeof(absolute),absolute,0);
    if(!length||length>=sizeof(absolute)){decision=-1;}
    else decision=copyout_overwrite(absolute,copyout_overwrite_context);
   }
   if(decision==2)fd=-2;
   else if(decision<=0){cpo_error=decision==0?"Copy cancelled":"Could not check the destination";fd=-1;}
   else fd=stage_output(dstname,existing);
  }
#elif __EMX__
  if (strcmp(dstname, "-") == 0) {
    _fsetmode(stdout, "b");
    fd = dup(STDOUT_FILENO);
  }
  else
    {
   int decision=1,exclusive=0;char absolute[_MAX_PATH];
   if(copyout_overwrite) {
    DWORD attributes=GetFileAttributesA(dstname),error=GetLastError();
    if(attributes!=INVALID_FILE_ATTRIBUTES) {
     DWORD length=GetFullPathNameA(dstname,sizeof(absolute),absolute,0);
     if(!length || length>=sizeof(absolute))strcpy(absolute,dstname);
     decision=copyout_overwrite(absolute,copyout_overwrite_context);
    } else if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)decision=-1;else exclusive=1;
   }
   if(decision==2)fd=-2;
   else if(decision<=0){cpo_error=decision==0?"Copy cancelled":"Could not check the destination";fd=-1;}
   else fd = open(dstname, O_BINARY | O_WRONLY | O_CREAT | (exclusive?O_EXCL:O_TRUNC), 0666);
  }
#else
  if (strcmp(dstname, "-") == 0)
    fd = dup(STDOUT_FILENO);
  else
    fd = open(dstname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif

  if (fd < 0 && fd != -2)
    {
      cpo_error = "error opening destination file";
      fd = -1;
    }

  if (dstname == hint)
    {
#ifdef WIN32
      _chdir(curdir);
#elif __EMX__
      _chdir2(curdir);
#else
      fchdir(dirfd);
      close(dirfd);
#endif
    }

#ifndef WIN32
  if(fd>=0)copyout_opened=1;
#endif /* Windows staged output owns its cleanup; callers must not delete the destination. */
  return fd;
#endif
}

/*
 * NAME:	openfiles()
 * DESCRIPTION:	open source and destination files
 */
static
int openfiles(hfsvol *vol, char *srcname, char *dstname, char *ext,
	      hfsfile **ifile, int *ofile)
{
  char *dsthint;

  copyout_skipped=0;copyout_opened=0;
  *ifile = opensrc(vol, srcname, &dsthint, ext);
  if (*ifile == 0)
    return -1;

  *ofile = opendst(dstname, dsthint);
  if (*ofile < 0)
    {
      hfs_close(*ifile);*ifile=0;copyout_skipped=(*ofile==-2);
      return *ofile==-2?0:-1;
    }

  return 0;
}

/*
 * NAME:	closefiles()
 * DESCRIPTION:	close source and destination files
 */
static
void closefiles(hfsfile *ifile, int ofile, int *result)
{
#ifdef WIN32
  if(*result==0&&(copyout_fault==2||_commit(ofile)<0)) {
   cpo_error="Error flushing completed output";*result=-1;
  }
#endif
  if (close(ofile) < 0 && *result == 0)
    {
      cpo_error = "error closing destination file";
      *result = -1;
    }

  if (hfs_close(ifile) < 0 && *result == 0)
    {
      cpo_error = hfs_error;
      *result = -1;
    }
#ifdef WIN32
  if(*result==0&&(copyout_fault==3||!MoveFileExA(copyout_temporary,copyout_destination,
       MOVEFILE_WRITE_THROUGH|(copyout_replace?MOVEFILE_REPLACE_EXISTING:0)))) {
   cpo_error="Could not publish completed output; previous destination retained";*result=-1;
  }
  if(*copyout_temporary){DeleteFileA(copyout_temporary);*copyout_temporary=0;}
#endif
  if(*result==0 && copyout_observer)copyout_observer(copyout_source,0,copyout_expected,1,copyout_context);
}

/* Interface Routines ====================================================== */

/*
 * NAME:	cpo->macb()
 * DESCRIPTION:	copy an HFS file to a UNIX file using MacBinary II translation
 */
static int cpo_macbinary(hfsvol *vol, char *srcname, char *dstname, int legacy)
{
  hfsfile *ifile;
  int ofile, result = 0;

  copyout_forks=3;
  if (openfiles(vol, srcname, dstname, ".bin", &ifile, &ofile) < 0)
    return -1;
  if(!ifile)return 0;

  result = do_macb(ifile, ofile, legacy);
  closefiles(ifile, ofile, &result);

  return result;
}

/* Both formats use .bin and identical 128-byte fork alignment. */
int cpo_macb(hfsvol *vol, char *srcname, char *dstname) {
  return cpo_macbinary(vol, srcname, dstname, 0);
}
int cpo_macb1(hfsvol *vol, char *srcname, char *dstname) {
  return cpo_macbinary(vol, srcname, dstname, 1);
}

/*
 * NAME:	cpo->binh()
 * DESCRIPTION:	copy an HFS file to a UNIX file using BinHex translation
 */
int cpo_binh(hfsvol *vol, char *srcname, char *dstname)
{
  hfsfile *ifile;
  int ofile, result;

  copyout_forks=3;
  if (openfiles(vol, srcname, dstname, ".hqx", &ifile, &ofile) < 0)
    return -1;
  if(!ifile)return 0;

  result = do_binh(ifile, ofile);
  closefiles(ifile, ofile, &result);

  return result;
}

/*
 * NAME:	cpo->text()
 * DESCRIPTION:	copy an HFS file to a UNIX file using text translation
 */
int cpo_text(hfsvol *vol, char *srcname, char *dstname)
{
  hfsfile *ifile;
  int ofile, result = 0;

  copyout_forks=1;
  if (openfiles(vol, srcname, dstname, ".txt", &ifile, &ofile) < 0)
    return -1;
  if(!ifile)return 0;

	if(crlf_translation) {
		setmode(ofile, O_TEXT);
	  result = do_text(ifile, ofile);
	} else {
		setmode(ofile, O_BINARY);
	  result = do_raw(ifile, ofile);
	}
  closefiles(ifile, ofile, &result);

  return result;
}

/*
 * NAME:	cpo->raw()
 * DESCRIPTION:	copy the data fork of an HFS file to a UNIX file
 */
int cpo_raw(hfsvol *vol, char *srcname, char *dstname)
{
  hfsfile *ifile;
  int ofile, result = 0;

  copyout_forks=1;
  if (openfiles(vol, srcname, dstname, 0, &ifile, &ofile) < 0)
    return -1;
  if(!ifile)return 0;

  result = do_raw(ifile, ofile);
  closefiles(ifile, ofile, &result);

  return result;
}

// LAURI
int cpo_raw_resource(hfsvol *vol, char *srcname, char *dstname)
{
  hfsfile *ifile;
  int ofile, result = 0;

  copyout_forks=2;
  if (openfiles(vol, srcname, dstname, 0, &ifile, &ofile) < 0)
    return -1;
  if(!ifile)return 0;

  if (hfs_setfork(ifile, 1) < 0) {
		result = -1;
	} else {
	  result = do_raw(ifile, ofile);
	}

  closefiles(ifile, ofile, &result);

  return result;
}
