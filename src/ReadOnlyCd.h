// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include "cache.h"
#include "CdImageToc.h"

// Byte-addressed, read-only view of the first classic HFS volume on a data CD.
// All device commands are fixed READ(10)/READ CAPACITY(10) commands.
class ReadOnlyCd {
public:
    ReadOnlyCd();
    ~ReadOnlyCd();
    bool OpenDrive(int drive);
    void Close();
    bool FindHfs();
    const std::vector<CdMedia::Track>& Tracks()const{return tracks_;}
    bool SelectDataTrack(unsigned number);
    bool SetReadRange(ULONGLONG begin,ULONGLONG size);
    ULONGLONG ReadBegin()const{return readBegin_;}
    ULONGLONG ReadEnd()const{return readEnd_;}
    bool ReadBytes(ULONGLONG offset, DWORD count, void* buffer);
    bool ReadVolume(ULONGLONG offset, DWORD count, void* buffer);
    HANDLE Handle() const { return handle_; }
    bool MediaGeneration(DWORD& out){if(!CheckIdentity()||!identity_)return false;out=generation_;return true;}
    DWORD Error() const { return error_; }
    ULONGLONG MediaSize() const { return mediaSize_; }
    ULONGLONG LengthQuerySize() const { return lengthSize_; }
    ULONGLONG TocQuerySize() const { return tocSize_; }
    ULONGLONG ScsiQuerySize() const { return scsiSize_; }
    DWORD ScsiCapacityError() const { return scsiCapacityError_; }
    DWORD VolumeSize() const { return volumeSize_; }
    DWORD DeclaredVolumeSize() const { return declaredVolumeSize_; }
    ULONGLONG VolumeOffset() const { return volumeOffset_; }
    const char* Transport() const { return passThroughUsed_ ? "SCSI READ(10)" : "ReadFile"; }
#ifdef HFV_CD_TESTING
    bool OpenFixture(const char* path, ULONGLONG reportedLength = 0);
    bool SetFixtureTracks(const std::vector<CdMedia::Track>& tracks);
    struct TestState {unsigned reads=0,checks=0,changeAfterRead=0;ULONGLONG bytes=0,minOffset=~ULONGLONG(0),maxEnd=0;DWORD generation=7,readError=0,failAbove=0;bool shortRead=false,noIdentity=false;} test;
#endif
private:
    ReadOnlyCd(const ReadOnlyCd&);
    ReadOnlyCd& operator=(const ReadOnlyCd&);
    bool ReadSectors(ULONGLONG offset, DWORD count, void* buffer);
    bool ScsiRead(DWORD sector, WORD sectors, void* buffer);
    bool ScsiCapacity();
    bool EnsureScsiHandle();
    bool FindHfsRange();
    bool LoadTracks();
    bool CheckVolume(ULONGLONG offset, ULONGLONG length, bool allowShortened = false);
    void SelectCapacity();
    bool CheckIdentity();
    bool ReadFailure(DWORD error);
    cachetype cache_={};DWORD generation_=0;bool identity_=false,cacheable_=true,invalid_=false;
    HANDLE handle_, scsiHandle_;
    BYTE* scratch_;
    ULONGLONG mediaSize_, volumeOffset_,readBegin_=0,readEnd_=0;
    std::vector<CdMedia::Track> tracks_;
    ULONGLONG lengthSize_, tocSize_, scsiSize_;
    DWORD scsiCapacityError_;
    DWORD volumeSize_, error_, declaredVolumeSize_;
    bool fixture_, passThroughUsed_;
    char devicePath_[16];
};
