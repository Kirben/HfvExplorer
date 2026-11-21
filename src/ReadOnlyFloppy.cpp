#include "DeviceWait.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ReadOnlyFloppy.h"
#include <stdio.h>
#include <string.h>

namespace {
const DWORD kSector=512,kTransfer=32*1024;
class WindowsFloppyReadApi : public FloppyReadApi {
public:
    UINT DriveType(int drive) override {char root[]="A:\\";root[0]+=char(drive);return GetDriveTypeA(root);}
    HANDLE Open(const char* path,DWORD access,DWORD sharing,DWORD flags) override {
        return HfvMediaOpen(path,access,sharing,NULL,OPEN_EXISTING,flags,NULL);
    }
    bool Geometry(HANDLE handle,DISK_GEOMETRY& geometry) override {
        DWORD returned=0;
        if(!HfvMediaControl(handle,IOCTL_DISK_GET_DRIVE_GEOMETRY,NULL,0,&geometry,sizeof(geometry),&returned,NULL)) return false;
        if(returned<sizeof(geometry)) {SetLastError(ERROR_INVALID_DATA);return false;}
        return true;
    }
    bool Verify(HANDLE handle,DWORD& changeCount) override {
        DWORD returned=0;changeCount=0;
        // This query and all other floppy IOCTLs in this module are read-only.
        return HfvMediaControl(handle,IOCTL_STORAGE_CHECK_VERIFY,NULL,0,&changeCount,sizeof(changeCount),&returned,NULL)!=FALSE;
    }
    bool Read(HANDLE handle,ULONGLONG offset,DWORD count,void* buffer,DWORD& got) override {
        LARGE_INTEGER position;position.QuadPart=offset;got=0;
        return HfvMediaSeek(handle,position,NULL,FILE_BEGIN) && HfvMediaRead(handle,buffer,count,&got,NULL);
    }
    void Close(HANDLE handle) override {HfvMediaClose(handle);}
};
WindowsFloppyReadApi nativeApi;
unsigned be16(const BYTE* p) {return unsigned(p[0])*256+p[1];}
DWORD be32(const BYTE* p) {return DWORD(be16(p))*65536+be16(p+2);}
}
FloppyReadApi& NativeFloppyReadApi() {return nativeApi;}
bool VerifyFloppyWriteMedia(HANDLE handle,DWORD& generation)
{
    DWORD returned=0;
    if(!HfvMediaControl(handle,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&generation,sizeof(generation),&returned,nullptr))
        return false;
    if(returned!=sizeof(generation)){
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    return true;
}


ReadOnlyFloppy::ReadOnlyFloppy(FloppyReadApi* api):api_(api?api:&nativeApi),handle_(INVALID_HANDLE_VALUE),
 scratch_(nullptr),geometry_(),size_(0),error_(0),changeCount_(0),hfs_(false),invalid_(false) {cache_init(&cache_);}
ReadOnlyFloppy::~ReadOnlyFloppy() {Close();}
void ReadOnlyFloppy::Close() {
    if(handle_!=INVALID_HANDLE_VALUE) api_->Close(handle_);
    if(scratch_) VirtualFree(scratch_,0,MEM_RELEASE);
    cache_final(&cache_);
    handle_=INVALID_HANDLE_VALUE;scratch_=nullptr;size_=error_=changeCount_=0;hfs_=invalid_=false;memset(&geometry_,0,sizeof(geometry_));
}
bool ReadOnlyFloppy::OpenFailure(DWORD error) {Close();error_=error;return false;}
bool ReadOnlyFloppy::ReadFailure(DWORD error) {cache_clear(&cache_);invalid_=true;error_=error?error:ERROR_READ_FAULT;return false;}
bool ReadOnlyFloppy::VerifySameMedia() {
    DWORD current=0;
    if(!api_->Verify(handle_,current)) return ReadFailure(GetLastError());
    if(current!=changeCount_) return ReadFailure(ERROR_MEDIA_CHANGED);
    return true;
}
bool ReadOnlyFloppy::OpenDrive(int drive) {
    Close();
    if(drive<0 || drive>25) return OpenFailure(ERROR_INVALID_PARAMETER);
    if(api_->DriveType(drive)!=DRIVE_REMOVABLE) return OpenFailure(ERROR_BAD_DEVICE);
    char device[16];sprintf(device,"\\\\.\\%c:",'A'+drive);
    handle_=api_->Open(device,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_RANDOM_ACCESS);
    if(handle_==INVALID_HANDLE_VALUE) return OpenFailure(GetLastError());
    if(!api_->Geometry(handle_,geometry_)) return OpenFailure(GetLastError());
    unsigned sectors=geometry_.MediaType==F3_720_512?9:geometry_.MediaType==F3_1Pt44_512?18:geometry_.MediaType==F3_2Pt88_512?36:0;
    // Do not classify ordinary removable disks/flash drives as floppies.
    if(!sectors || geometry_.Cylinders.QuadPart!=80 || geometry_.TracksPerCylinder!=2 ||
       geometry_.SectorsPerTrack!=sectors || geometry_.BytesPerSector!=kSector)
        return OpenFailure(ERROR_NOT_SUPPORTED);
    size_=80*2*sectors*kSector;
    if(!api_->Verify(handle_,changeCount_)) return OpenFailure(GetLastError());
    scratch_=static_cast<BYTE*>(VirtualAlloc(NULL,kTransfer,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!scratch_) return OpenFailure(ERROR_NOT_ENOUGH_MEMORY);
    return true;
}
bool ReadOnlyFloppy::ReadBytes(ULONGLONG offset,DWORD count,void* bytes) {
    if(handle_==INVALID_HANDLE_VALUE || !scratch_) {error_=ERROR_INVALID_HANDLE;return false;}
    if(invalid_) return false;
    if(!bytes && count) {error_=ERROR_INVALID_PARAMETER;return false;}
    if(offset>size_ || count>size_-offset) {error_=ERROR_HANDLE_EOF;return false;}
    if(!count) return true;
    // Original floppy_read policy: check cached 512-byte sectors first,
    // then batch the remaining contiguous miss. FLOPPY_READ_AHEAD_SECTORS
    // was 1, so do not fetch unrelated tracks. One media check covers this
    // logical request; validate each new physical transfer before caching it.
    if(!VerifySameMedia())return false;
    BYTE* out=static_cast<BYTE*>(bytes);BYTE sector[kSector];
    while(count) {
        DWORD block=DWORD(offset/kSector),skip=DWORD(offset%kSector);
        if(cache_get(&cache_,block,(char*)sector)){
            DWORD chunk=min(count,kSector-skip);
            memcpy(out,sector+skip,chunk);out+=chunk;offset+=chunk;count-=chunk;continue;
        }
        ULONGLONG aligned=offset-skip;
        DWORD chunk=min(count,kTransfer-skip);
        DWORD transfer=(skip+chunk+kSector-1)&~(kSector-1),got=0;
        if(!api_->Read(handle_,aligned,transfer,scratch_,got))return ReadFailure(GetLastError());
        if(got!=transfer)return ReadFailure(ERROR_READ_FAULT);
        if(!VerifySameMedia())return false;
        for(DWORD n=0;n<transfer/kSector;++n)cache_put(&cache_,block+n,(char*)scratch_+n*kSector,kSector);
        memcpy(out,scratch_+skip,chunk);out+=chunk;offset+=chunk;count-=chunk;
    }
    error_=ERROR_SUCCESS;return true;
}
bool ReadOnlyFloppy::FindHfs() {
    hfs_=false;BYTE mdb[kSector];
    if(!ReadBytes(1024,sizeof(mdb),mdb)) return false;
    unsigned blocks=be16(mdb+18),first=be16(mdb+28),bitmap=be16(mdb+14);DWORD allocation=be32(mdb+20);
    if(be16(mdb)!=0x4244 || !blocks || allocation<512 || allocation%512 || !mdb[36] || mdb[36]>27 ||
       bitmap<3 || first<bitmap || ULONGLONG(bitmap)*512+(blocks+7)/8>ULONGLONG(first)*512 ||
       ULONGLONG(first)*512+ULONGLONG(blocks)*allocation>size_) {error_=ERROR_UNRECOGNIZED_VOLUME;return false;}
    for(unsigned tree=0;tree<2;++tree) {
        unsigned field=130+tree*16;DWORD length=be32(mdb+field);unsigned count=0;
        for(unsigned e=0;e<3;++e) {
            unsigned start=be16(mdb+field+4+4*e),n=be16(mdb+field+6+4*e);
            if(n && (start>=blocks || n>blocks-start || n>blocks-count)) {error_=ERROR_UNRECOGNIZED_VOLUME;return false;}
            count+=n;
        }
        if(length<512 || !count || length>ULONGLONG(blocks)*allocation) {error_=ERROR_UNRECOGNIZED_VOLUME;return false;}
    }
    hfs_=true;return true;
}
bool ReadOnlyFloppy::ReadVolume(ULONGLONG offset,DWORD count,void* bytes) {
    if(!hfs_) {error_=ERROR_UNRECOGNIZED_VOLUME;return false;}
    return ReadBytes(offset,count,bytes);
}
