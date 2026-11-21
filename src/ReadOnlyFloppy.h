// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <winioctl.h>
#include "cache.h"

// Deliberately read-only OS boundary. Tests supply this interface instead of
// opening devices; there is no write/format/eject operation to substitute.
class FloppyReadApi {
public:
    virtual ~FloppyReadApi() {}
    virtual UINT DriveType(int drive)=0;
    virtual HANDLE Open(const char* path,DWORD access,DWORD sharing,DWORD flags)=0;
    virtual bool Geometry(HANDLE handle,DISK_GEOMETRY& geometry)=0;
    virtual bool Verify(HANDLE handle,DWORD& changeCount)=0;
    virtual bool Read(HANDLE handle,ULONGLONG offset,DWORD count,void* buffer,DWORD& got)=0;
    virtual void Close(HANDLE handle)=0;
};
FloppyReadApi& NativeFloppyReadApi();
bool VerifyFloppyWriteMedia(HANDLE,DWORD& generation);
class ReadOnlyFloppy {
public:
    explicit ReadOnlyFloppy(FloppyReadApi* api=nullptr);
    ~ReadOnlyFloppy();
    bool OpenDrive(int drive);
    void Close();
    bool FindHfs();
    bool ReadBytes(ULONGLONG offset,DWORD count,void* bytes);
    bool ReadVolume(ULONGLONG offset,DWORD count,void* bytes);
    HANDLE Handle() const {return handle_;}
    DWORD Error() const {return error_;}
    DWORD Generation() const {return changeCount_;}
    DWORD MediaSize() const {return size_;}
    DWORD VolumeSize() const {return hfs_?size_:0;}
    const DISK_GEOMETRY& MediaGeometry() const {return geometry_;}
private:
    ReadOnlyFloppy(const ReadOnlyFloppy&)=delete;
    ReadOnlyFloppy& operator=(const ReadOnlyFloppy&)=delete;
    bool OpenFailure(DWORD error);
    bool ReadFailure(DWORD error);
    bool VerifySameMedia();
    cachetype cache_;
    FloppyReadApi* api_;
    HANDLE handle_;
    BYTE* scratch_;
    DISK_GEOMETRY geometry_;
    DWORD size_,error_,changeCount_;
    bool hfs_,invalid_;
};
