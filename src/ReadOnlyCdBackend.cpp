// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "ReadOnlyCd.h"
#include "ReadOnlyCdBackend.h"
#include "floppy.h"
#define _FHOOK_INTERNAL_
#include "filehook.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

namespace {
ReadOnlyCd cds[MAX_DEVICES];
const int kFdBase = 0x10000000, kFdCount = 128;
struct Descriptor { bool open; int drive; long position; } descriptors[kFdCount] = {};
#ifdef HFV_CD_TESTING
char fixtures[MAX_DEVICES][MAX_PATH] = {};
#endif
int pathDrive(const char* path) {
    if (!path || strlen(path) != sizeof("A:\\NP~1_2~3.DSK") - 1 || path[1] != ':') return -1;
    int drive = toupper((unsigned char)path[0]) - 'A';
    return drive >= 0 && drive < MAX_DEVICES && !_stricmp(path + 1, ":\\NP~1_2~3.DSK") ? drive : -1;
}
Descriptor* descriptor(int fd) {
    return fd >= kFdBase && fd < kFdBase + kFdCount && descriptors[fd-kFdBase].open ? &descriptors[fd-kFdBase] : NULL;
}
bool virtualFd(int fd) { return fd >= kFdBase && fd < kFdBase + kFdCount; }
int hookedStat(const char* path, struct stat* st) {
    if (pathDrive(path) >= 0) { errno = ENOENT; return -1; }
    return stat(path, st);
}
int hookedOpen(const char* path, int flags, ...) {
    int drive = pathDrive(path);
    if (drive < 0) {
        if (flags & _O_CREAT) { va_list ap; va_start(ap, flags); int mode = va_arg(ap,int); va_end(ap); return _open(path,flags,mode); }
        return _open(path, flags);
    }
    if (flags & (_O_WRONLY|_O_RDWR|_O_CREAT|_O_TRUNC|_O_APPEND)) { errno = EROFS; return -1; }
    for (int n=0; n<kFdCount; ++n) if (!descriptors[n].open) {
        if (!cd_init(drive, TRUE)) { errno = EIO; return -1; }
        descriptors[n].drive=drive; descriptors[n].position=0; descriptors[n].open=true;
        return kFdBase+n;
    }
    errno=EMFILE; return -1;
}
int hookedClose(int fd) {
    Descriptor* d=descriptor(fd);
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _close(fd); }
    cd_final(d->drive); d->open=false; return 0;
}
int hookedRead(int fd, void* buffer, unsigned count) {
    Descriptor* d=descriptor(fd);
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _read(fd,buffer,count); }
    if (count > INT_MAX) { errno=EINVAL; return -1; }
    DWORD available=cds[d->drive].VolumeSize() - d->position;
    count=min(count,available);
    if (!cds[d->drive].ReadVolume(d->position,count,buffer)) { errno=EIO; return -1; }
    d->position+=count; return count;
}
int hookedWrite(int fd, const void* buffer, unsigned count) {
    if (virtualFd(fd)) { errno=EROFS; return -1; }
    return _write(fd,buffer,count);
}
long hookedSeek(int fd, long offset, int origin) {
    Descriptor* d=descriptor(fd);
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _lseek(fd,offset,origin); }
    LONGLONG position=offset;
    if (origin==SEEK_CUR) position+=d->position;
    else if (origin==SEEK_END) position+=cds[d->drive].VolumeSize();
    else if (origin!=SEEK_SET) { errno=EINVAL; return -1; }
    if (position < 0 || position > cds[d->drive].VolumeSize()) { errno=EINVAL; return -1; }
    d->position=(long)position; return d->position;
}
}
extern "C" {
void readonly_cd_backend_init(void) {
    fhook_init_fake_funcs(hookedStat,hookedOpen,hookedClose,hookedRead,hookedWrite,hookedSeek);
    fhook_set_fake_funcs();
}
void readonly_cd_backend_final(void) {
    for(int n=0;n<kFdCount;++n) if(descriptors[n].open) hookedClose(kFdBase+n);
    for(int n=0;n<MAX_DEVICES;++n) { cds[n].Close(); flcds[n].handle=NULL; flcds[n].refcount=0; }
    fhook_set_real_funcs();
}
HANDLE cd_init(int drive, BOOL) {
    if(drive<0 || drive>=MAX_DEVICES) return NULL;
    if(flcds[drive].refcount) { ++flcds[drive].refcount; return flcds[drive].handle; }
    bool opened;
#ifdef HFV_CD_TESTING
    if(fixtures[drive][0]) opened=cds[drive].OpenFixture(fixtures[drive]); else
#endif
    opened=cds[drive].OpenDrive(drive);
    if(!opened || !cds[drive].FindHfs()) { cds[drive].Close(); return NULL; }
    flcds[drive].handle=cds[drive].Handle(); flcds[drive].refcount=1;
    flcds[drive].type=FLTYPE_CD; flcds[drive].sector_size=2048;
    flcds[drive].volume_start=(ULONG)cds[drive].VolumeOffset();
    return flcds[drive].handle;
}
void cd_final(int drive) {
    if(drive<0 || drive>=MAX_DEVICES || !flcds[drive].refcount) return;
    if(--flcds[drive].refcount==0) { cds[drive].Close(); flcds[drive].handle=NULL; }
}
long get_cd_size(HANDLE h,int drive) {
    return drive>=0 && drive<MAX_DEVICES && h==flcds[drive].handle ? (long)cds[drive].VolumeSize() : 0;
}
int cd_read(HANDLE h,int drive,ULONG offset,int count,char* buffer,BOOL is_cd) {
    if(!is_cd || drive<0 || drive>=MAX_DEVICES || !h || h!=flcds[drive].handle || count<0) return 0;
    return cds[drive].ReadVolume(offset,count,buffer) ? count : 0;
}
int try_mount_cd(HANDLE h,int drive) { return h && get_cd_size(h,drive)>0; }
int is_hfs_cd_present(int drive) {
    HANDLE h=cd_init(drive,TRUE); if(!h) return 0;
    cd_final(drive); return 1;
}
void get_cd_volume_file_name(int drive,char* name) { sprintf(name,"%c:\\NP~1_2~3.DSK",'A'+drive); }
}
#ifdef HFV_CD_TESTING
bool readonly_cd_attach_fixture(int drive,const char* path) {
    if(drive<0 || drive>=MAX_DEVICES || flcds[drive].refcount || strlen(path)>=MAX_PATH) return false;
    strcpy(fixtures[drive],path); return true;
}
#endif
