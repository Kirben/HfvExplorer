// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "ReadOnlyCd.h"
#include "ReadOnlyCdBackend.h"
#include "ReadOnlyFloppy.h"
#include "DirectFloppy.h"
#include "ReadOnlySource.h"
#include "ImageHandleTransfer.h"
#include "ClassicImagePath.h"
#include <io.h>
#include <fcntl.h>
#include "floppy.h"
#define _FHOOK_INTERNAL_
#include "filehook.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <mutex>
#include <memory>
#include <vector>

namespace {
std::recursive_mutex deviceMutex[MAX_DEVICES];
std::mutex descriptorMutex;
// Invalid inputs use a separate lock and are rejected by the API as before.
std::recursive_mutex invalidDeviceMutex;
std::recursive_mutex& deviceLock(int drive){return drive>=0&&drive<MAX_DEVICES?deviceMutex[drive]:invalidDeviceMutex;}
ReadOnlyCd cds[MAX_DEVICES];
ReadOnlyFloppy* floppies[MAX_DEVICES] = {};
#ifdef HFV_FLOPPY_TESTING
FloppyReadApi* floppyApis[MAX_DEVICES] = {};
#endif
const int kFdBase = 0x10000000, kFdCount = 128;
struct Descriptor { int drive=-1; hfv_offset_t position=0; bool floppy=false; bool direct=false; bool writable=false;std::shared_ptr<HfsPlus::Source> source;std::recursive_mutex mutex; };
std::recursive_mutex& descriptorLock(const std::shared_ptr<Descriptor>& d){return d->source?d->mutex:deviceLock(d->drive);}
std::shared_ptr<Descriptor> descriptors[kFdCount];
#ifdef HFV_CD_TESTING
char fixtures[MAX_DEVICES][MAX_PATH] = {};
#endif
int pathDrive(const char* path) {
    if (!path || strlen(path) != sizeof("A:\\NP~1_2~3.DSK") - 1 || path[1] != ':') return -1;
    int drive = toupper((unsigned char)path[0]) - 'A';
    return drive >= 0 && drive < MAX_DEVICES &&
        (!_stricmp(path + 1, ":\\NP~1_2~3.DSK") || !_stricmp(path + 1, ":\\NO~1_2~3.DSK")) ? drive : -1;
}
std::shared_ptr<Descriptor> descriptor(int fd) {
    std::lock_guard<std::mutex> slots(descriptorMutex);
    return fd>=kFdBase&&fd<kFdBase+kFdCount?descriptors[fd-kFdBase]:nullptr;
}
bool ensureFloppy(int drive) {
    if(!floppies[drive]) return false;
    if(floppies[drive]->Handle()!=INVALID_HANDLE_VALUE) return true;
    return floppies[drive]->OpenDrive(drive)&&floppies[drive]->FindHfs();
}
bool virtualFd(int fd) { return fd >= kFdBase && fd < kFdBase + kFdCount; }
int hookedStat(const char* path, struct _stat64* st) {
    if (ReadOnlySources::IsPath(path)||pathDrive(path) >= 0) { errno = ENOENT; return -1; }
    return fhook_stat_image(path,st);
}
int hookedOpen(const char* path, int flags, ...) {
    if(ReadOnlySources::IsPath(path)){
        if(flags&(_O_CREAT|_O_TRUNC|_O_APPEND|_O_WRONLY|_O_RDWR)){errno=EROFS;return -1;}auto source=ReadOnlySources::Find(path);if(!source){errno=ENOENT;return -1;}
        try{source->CheckMedia();}catch(...){errno=EIO;return -1;}auto d=std::make_shared<Descriptor>();d->source=std::move(source);std::lock_guard<std::mutex> slots(descriptorMutex);for(int n=0;n<kFdCount;++n)if(!descriptors[n]){descriptors[n]=d;return kFdBase+n;}errno=EMFILE;return -1;
    }
    int drive = pathDrive(path);
    if (drive < 0) {
        if(!(flags&(_O_CREAT|_O_TRUNC|_O_APPEND))){
            try{
                std::wstring wide;
                if(ClassicImagePath::IsEncoded(path))wide=ClassicImagePath::Decode(path);
                else{
                    const auto count=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);
                    if(count<=0){errno=EINVAL;return -1;}
                    wide.resize(count);
                    MultiByteToWideChar(CP_ACP,0,path,-1,wide.data(),count);
                }
                const auto handle=ImageHandleTransfer::Open(wide.c_str(),(flags&(_O_RDWR|_O_WRONLY))!=0);
                if(handle!=INVALID_HANDLE_VALUE){
                    const auto fd=_open_osfhandle(intptr_t(handle),flags|_O_BINARY);
                    if(fd<0)CloseHandle(handle);
                    return fd;
                }
            }catch(...){errno=EACCES;return -1;}
        }
        if (flags & _O_CREAT) { va_list ap; va_start(ap, flags); int mode = va_arg(ap,int); va_end(ap); return fhook_open_image(path,flags,mode); }
        return fhook_open_image(path, flags);
    }
    bool direct=physical_floppy_path(path)>=0 && DirectFloppy::Current(drive);
    if((flags&(_O_CREAT|_O_TRUNC|_O_APPEND))||(!direct&&(flags&(_O_WRONLY|_O_RDWR)))){errno=EROFS;return -1;}
    std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    bool floppy=toupper((unsigned char)path[4])=='O';
    if (!direct && !(floppy?floppy_init(drive,TRUE):cd_init(drive, TRUE))) { errno = EIO; return -1; }
    auto value=std::make_shared<Descriptor>();value->floppy=floppy;value->direct=direct;value->writable=direct&&(flags&(_O_WRONLY|_O_RDWR));value->drive=drive;value->position=0;
    {
     std::lock_guard<std::mutex> slots(descriptorMutex);
     for(int n=0;n<kFdCount;++n)if(!descriptors[n]){descriptors[n]=value;return kFdBase+n;}
    }
    if(!direct){if(floppy)floppy_final(drive);else cd_final(drive);}

    errno=EMFILE; return -1;
}
int hookedClose(int fd) {
    auto d=descriptor(fd);
    std::unique_lock<std::recursive_mutex> device;if(d)device=std::unique_lock<std::recursive_mutex>(descriptorLock(d));
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _close(fd); }
    {std::lock_guard<std::mutex> slots(descriptorMutex);if(descriptors[fd-kFdBase]!=d){errno=EBADF;return -1;}descriptors[fd-kFdBase].reset();}
    if(!d->source&&!d->direct){if(d->floppy)floppy_final(d->drive);else cd_final(d->drive);}return 0;
}
int hookedIdentity(int fd,const char* path) {
    auto d=descriptor(fd);if(!d)return virtualFd(fd)?0:-1;
    // A drive letter is not a media identity; force a fresh removable-media probe.
    (void)path;return 0;
}
int hookedSync(int fd) {
    auto d=descriptor(fd);
    std::unique_lock<std::recursive_mutex> device;if(d)device=std::unique_lock<std::recursive_mutex>(descriptorLock(d));
    if(d&&descriptor(fd)!=d){errno=EBADF;return -1;}
    if(!d){if(virtualFd(fd)){errno=EBADF;return -1;}return _commit(fd);}
    if(!d->direct||!d->writable){errno=EROFS;return -1;}
    auto disk=DirectFloppy::Current(d->drive);
    if(!disk){errno=EBADF;return -1;}
    if(!disk->Finish()){errno=EIO;return -1;}
    return 0;
}
int hookedRead(int fd, void* buffer, unsigned count) {
    auto d=descriptor(fd);
    std::unique_lock<std::recursive_mutex> device;if(d)device=std::unique_lock<std::recursive_mutex>(descriptorLock(d));
    if(d&&descriptor(fd)!=d){errno=EBADF;return -1;}
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _read(fd,buffer,count); }
    if (count > INT_MAX) { errno=EINVAL; return -1; }
    if(d->source){try{auto size=d->source->Size();if(uint64_t(d->position)>size){errno=EINVAL;return -1;}count=unsigned((std::min)(uint64_t(count),size-uint64_t(d->position)));d->source->Read(uint64_t(d->position),buffer,count);d->position+=count;return int(count);}catch(...){errno=EIO;return -1;}}
    if(d->direct){auto disk=DirectFloppy::Current(d->drive);if(!disk){errno=EBADF;return -1;}count=min(count,1474560u-DWORD(d->position));if(!disk->Read(d->position,count,buffer)){errno=EIO;return -1;}d->position+=count;return count;}
    if(d->floppy&&!ensureFloppy(d->drive)){errno=EIO;return -1;}
    DWORD available=(d->floppy?floppies[d->drive]->VolumeSize():cds[d->drive].VolumeSize()) - d->position;
    count=min(count,available);
    if (!(d->floppy?floppies[d->drive]->ReadVolume(d->position,count,buffer):cds[d->drive].ReadVolume(d->position,count,buffer))) { errno=EIO; return -1; }
    d->position+=count; return count;
}
int hookedWrite(int fd, const void* buffer, unsigned count) {
    auto d=descriptor(fd);
    std::unique_lock<std::recursive_mutex> device;if(d)device=std::unique_lock<std::recursive_mutex>(descriptorLock(d));
    if(d&&descriptor(fd)!=d){errno=EBADF;return -1;}
    if(d&&d->direct&&d->writable){auto disk=DirectFloppy::Current(d->drive);if(!disk){errno=EBADF;return -1;}if(count>INT_MAX||!disk->Write(d->position,count,buffer)){errno=EIO;return -1;}d->position+=count;return count;}
    if (virtualFd(fd)) { errno=EROFS; return -1; }
    return _write(fd,buffer,count);
}
hfv_offset_t hookedSeek(int fd, hfv_offset_t offset, int origin) {
    auto d=descriptor(fd);
    std::unique_lock<std::recursive_mutex> device;if(d)device=std::unique_lock<std::recursive_mutex>(descriptorLock(d));
    if(d&&descriptor(fd)!=d){errno=EBADF;return -1;}
    if (!d) { if (virtualFd(fd)) { errno=EBADF; return -1; } return _lseeki64(fd,offset,origin); }
    if(d->source){try{d->source->CheckMedia();auto size=d->source->Size();LONGLONG base=origin==SEEK_SET?0:origin==SEEK_CUR?d->position:origin==SEEK_END?LONGLONG(size):-1;if(base<0||offset< -base||offset>LONGLONG(size)-base){errno=EINVAL;return -1;}return d->position=base+offset;}catch(...){errno=EIO;return -1;}}
    if(d->direct&&!DirectFloppy::Current(d->drive)){errno=EBADF;return -1;}
    if(!d->direct&&d->floppy&&!ensureFloppy(d->drive)){errno=EIO;return -1;}
    LONGLONG size=d->direct?1474560:d->floppy?floppies[d->drive]->VolumeSize():cds[d->drive].VolumeSize();
    LONGLONG base=origin==SEEK_SET?0:origin==SEEK_CUR?d->position:origin==SEEK_END?size:-1;
    if(base<0 || offset < -base || offset > size-base) { errno=EINVAL; return -1; }
    LONGLONG position=base+offset;
    d->position=position; return d->position;
}
}
extern "C" {
void readonly_cd_backend_init(void) {
    fhook_init_fake_funcs(hookedStat,hookedOpen,hookedClose,hookedRead,hookedWrite,hookedSeek);
    fhook_init_sync(hookedSync);
    fhook_init_identity(hookedIdentity);
    fhook_set_fake_funcs();
}
void readonly_cd_backend_final(void) {
    for(int n=0;n<kFdCount;++n) if(descriptor(kFdBase+n)) hookedClose(kFdBase+n);
    for(int n=0;n<MAX_DEVICES;++n) { cds[n].Close(); flcds[n].handle=NULL; flcds[n].refcount=0; }
    for(int n=0;n<MAX_DEVICES;++n) { delete floppies[n]; floppies[n]=NULL; }
    fhook_set_real_funcs();
}
HANDLE cd_init(int drive, BOOL) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES) return NULL;
    if(flcds[drive].refcount) { if(flcds[drive].type!=FLTYPE_CD) return NULL; ++flcds[drive].refcount; return flcds[drive].handle; }
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
void cd_final(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES || flcds[drive].type!=FLTYPE_CD || !flcds[drive].refcount) return;
    if(--flcds[drive].refcount==0) { cds[drive].Close(); flcds[drive].handle=NULL; }
}
long get_cd_size(HANDLE h,int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    return drive>=0 && drive<MAX_DEVICES && h==flcds[drive].handle ? (long)cds[drive].VolumeSize() : 0;
}
int cd_read(HANDLE h,int drive,ULONG offset,int count,char* buffer,BOOL is_cd) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(!is_cd || drive<0 || drive>=MAX_DEVICES || !h || h!=flcds[drive].handle || count<0) return 0;
    return cds[drive].ReadVolume(offset,count,buffer) ? count : 0;
}
int try_mount_cd(HANDLE h,int drive) { return h && get_cd_size(h,drive)>0; }
int is_hfs_cd_present(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    HANDLE h=cd_init(drive,TRUE); if(!h) return 0;
    cd_final(drive); return 1;
}
void get_cd_volume_file_name(int drive,char* name) { sprintf(name,"%c:\\NP~1_2~3.DSK",'A'+drive); }
int readonly_media_path(const char* path) {int drive=pathDrive(path);return drive>=0&&!(physical_floppy_path(path)>=0&&DirectFloppy::Current(drive));}
int physical_floppy_writable(const char* path){int drive=physical_floppy_path(path);return drive>=0&&DirectFloppy::Current(drive);}
int physical_floppy_path(const char* path) {
    int drive=pathDrive(path);return drive>=0&&toupper((unsigned char)path[4])=='O'?drive:-1;
}
void physical_floppy_suspend(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive>=0&&drive<MAX_DEVICES&&floppies[drive]) floppies[drive]->Close();
}
void native_media_suspend(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0||drive>=MAX_DEVICES)return;
    std::vector<int> closing;{std::lock_guard<std::mutex> slots(descriptorMutex);for(int n=0;n<kFdCount;++n)if(descriptors[n]&&descriptors[n]->drive==drive)closing.push_back(kFdBase+n);}
    for(int fd:closing)hookedClose(fd);
    cds[drive].Close();if(floppies[drive])floppies[drive]->Close();
    flcds[drive].handle=NULL;flcds[drive].refcount=0;
}
HANDLE floppy_init(int drive,BOOL) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES) return NULL;
    if(flcds[drive].refcount) {if(flcds[drive].type!=FLTYPE_FLOPPY) return NULL; ++flcds[drive].refcount;return flcds[drive].handle;}
    if(!floppies[drive]) {
#ifdef HFV_FLOPPY_TESTING
        if(!floppyApis[drive]) return NULL; // Tests never fall through to hardware.
        floppies[drive]=new ReadOnlyFloppy(floppyApis[drive]);
#else
        floppies[drive]=new ReadOnlyFloppy();
#endif
    }
    if(!floppies[drive]->OpenDrive(drive) || !floppies[drive]->FindHfs()) {floppies[drive]->Close();return NULL;}
    // Stable adapter identity survives reopening the native handle.
    flcds[drive].handle=reinterpret_cast<HANDLE>(floppies[drive]);flcds[drive].refcount=1;flcds[drive].type=FLTYPE_FLOPPY;
    flcds[drive].sector_size=512;flcds[drive].volume_start=0;return flcds[drive].handle;
}
void floppy_final(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES || flcds[drive].type!=FLTYPE_FLOPPY || !flcds[drive].refcount) return;
    if(--flcds[drive].refcount==0) {floppies[drive]->Close();flcds[drive].handle=NULL;}
}
long get_floppy_size(HANDLE handle,int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    return drive>=0 && drive<MAX_DEVICES && floppies[drive] && flcds[drive].type==FLTYPE_FLOPPY &&
        handle && handle==flcds[drive].handle && ensureFloppy(drive)?(long)floppies[drive]->VolumeSize():0;
}
int floppy_read(HANDLE handle,int drive,ULONG offset,int count,char* buffer) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(!get_floppy_size(handle,drive) || count<0) return 0;
    if(floppies[drive]->ReadVolume(offset,(DWORD)count,buffer)) return count;
    SetLastError(floppies[drive]->Error());return 0;
}
int floppy_write(HANDLE,int,ULONG,int,char*) {SetLastError(ERROR_WRITE_PROTECT);return 0;}
void get_floppy_volume_file_name(int drive,char* name) {sprintf(name,"%c:\\NO~1_2~3.DSK",'A'+drive);}
int is_hfs_floppy_present(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));HANDLE handle=floppy_init(drive,TRUE);if(!handle) return 0;floppy_final(drive);return 1;}
int is_any_floppy_present(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES) return 0;
#ifdef HFV_FLOPPY_TESTING
    if(!floppyApis[drive]) return 0;
    ReadOnlyFloppy device(floppyApis[drive]);
#else
    ReadOnlyFloppy device;
#endif
    return device.OpenDrive(drive)?1:0;
}
int physical_cd_generation(int drive,unsigned long* generation){std::lock_guard<std::recursive_mutex> lock(deviceLock(drive));if(drive<0||drive>=MAX_DEVICES||!generation||!cd_init(drive,TRUE))return 0;bool ok=cds[drive].MediaGeneration(*generation);cd_final(drive);return ok?1:0;}
int physical_floppy_generation(int drive,unsigned long* generation){std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
 if(drive<0||drive>=MAX_DEVICES||!generation)return 0;
 if(auto writer=DirectFloppy::Current(drive)){*generation=writer->Generation();return 1;}
 if(!floppies[drive]||floppies[drive]->Handle()==INVALID_HANDLE_VALUE)return 0;
 *generation=floppies[drive]->Generation();return 1;
}
int get_floppy_max_size(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));return drive>=0 && drive<MAX_DEVICES && floppies[drive]?(int)floppies[drive]->MediaSize():0;}
void floppy_get_geometry(int drive,int* c,int* h,int* s,int* bytes) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    *c=*h=*s=*bytes=0;
    if(drive<0 || drive>=MAX_DEVICES || !floppies[drive]) return;
    const DISK_GEOMETRY& g=floppies[drive]->MediaGeometry();
    *c=(int)g.Cylinders.QuadPart;*h=(int)g.TracksPerCylinder;*s=(int)g.SectorsPerTrack;*bytes=(int)g.BytesPerSector;
}
}
#ifdef HFV_FLOPPY_TESTING
FloppyReadApi* physical_floppy_test_api(int drive) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));return drive>=0&&drive<MAX_DEVICES?floppyApis[drive]:nullptr;}
bool readonly_floppy_attach_api(int drive,FloppyReadApi* api) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES || flcds[drive].refcount) return false;
    delete floppies[drive];floppies[drive]=NULL;floppyApis[drive]=api;return true;
}
#endif
#ifdef HFV_CD_TESTING
bool readonly_cd_attach_fixture(int drive,const char* path) {std::lock_guard<std::recursive_mutex> deviceGuard(deviceLock(drive));
    if(drive<0 || drive>=MAX_DEVICES || flcds[drive].refcount || strlen(path)>=MAX_PATH) return false;
    strcpy(fixtures[drive],path); return true;
}
#endif
