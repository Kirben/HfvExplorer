// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ReadOnlyFloppy.h"
#include <vector>
#include <string>
#include <string.h>
#include <stdint.h>
class FakeFloppy : public FloppyReadApi {
public:
    std::vector<BYTE> media;
    DISK_GEOMETRY geometry={};
    UINT driveType=DRIVE_REMOVABLE;
    DWORD changeCount=7,openError=0,geometryError=0,verifyError=0,readError=0;
    unsigned opens=0,closes=0,reads=0,verifies=0;
    ULONGLONG requestedReadBytes=0;
    unsigned changeAfterRead=0;
    bool shortRead=false,aligned=true,readOnly=true;
    std::string devicePath;
    explicit FakeFloppy(const std::vector<BYTE>& bytes):media(bytes) {
        geometry.Cylinders.QuadPart=80;geometry.TracksPerCylinder=2;geometry.SectorsPerTrack=18;
        geometry.BytesPerSector=512;geometry.MediaType=F3_1Pt44_512;
    }
    UINT DriveType(int) override {return driveType;}
    HANDLE Open(const char* path,DWORD access,DWORD sharing,DWORD flags) override {
        ++opens;devicePath=path;
        readOnly &= access==GENERIC_READ && sharing==(FILE_SHARE_READ|FILE_SHARE_WRITE) && flags==FILE_FLAG_RANDOM_ACCESS;
        if(openError) {SetLastError(openError);return INVALID_HANDLE_VALUE;}
        return reinterpret_cast<HANDLE>(uintptr_t(0x12345000));
    }
    bool Geometry(HANDLE,DISK_GEOMETRY& out) override {
        if(geometryError) {SetLastError(geometryError);return false;}out=geometry;return true;
    }
    bool Verify(HANDLE,DWORD& out) override {
        ++verifies;if(verifyError) {SetLastError(verifyError);return false;}out=changeCount;return true;
    }
    bool Read(HANDLE,ULONGLONG offset,DWORD count,void* buffer,DWORD& got) override {
        ++reads;requestedReadBytes+=count;aligned &= offset%512==0 && count%512==0 && uintptr_t(buffer)%512==0;
        got=0;if(readError) {SetLastError(readError);return false;}
        if(offset>media.size() || count>media.size()-offset) {SetLastError(ERROR_HANDLE_EOF);return false;}
        got=shortRead?count-1:count;memcpy(buffer,media.data()+size_t(offset),got);
        if(changeAfterRead==reads) ++changeCount;
        return true;
    }
    void Close(HANDLE) override {++closes;}
};
