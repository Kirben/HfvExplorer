// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ReadOnlyFloppy.h"
#ifdef HFV_APP_REMOVABLE_UNDO
#include "RemovableUndo.h"
#endif

#include <vector>
#include <optional>
#include <string>

class FloppyWriteApi : public FloppyReadApi {
public:
    virtual bool Lock(HANDLE)=0;
    virtual bool Dismount(HANDLE)=0;
    virtual bool Unlock(HANDLE)=0;
    virtual bool FormatTracks1440(HANDLE)=0;
    virtual bool Write(HANDLE,ULONGLONG,DWORD,const void*,DWORD&)=0;
    virtual bool Flush(HANDLE)=0;
};
#ifdef HFV_APP_REMOVABLE_UNDO
namespace FloppyRecovery {
inline constexpr RemovableUndo::Range Range{0,1474560,512};
void Read(FloppyWriteApi&,HANDLE,DWORD generation,uint64_t,DWORD,void*);
void Replay(FloppyWriteApi&,HANDLE,DWORD generation,int drive);
}
#endif
typedef void (*FloppyProgress)(const char* phase,unsigned completed,unsigned total,void* context);
class FloppyWriter {
public:
    explicit FloppyWriter(FloppyWriteApi* api=nullptr);
    // Without expectedOriginal, write/verify the complete 1.44 MB image.
    // With it, compare the whole original disk under lock, then write/verify
    // only tracks whose bytes differ. No low-level format in this mode.
    bool WriteImage(int drive,const std::vector<BYTE>& image,bool formatTracks=false,
                    FloppyProgress progress=nullptr,void* context=nullptr,
                    const std::vector<BYTE>* expectedOriginal=nullptr,std::optional<DWORD> expectedGeneration={});
    DWORD Error() const {return error_;}
    DWORD FlushError() const {return flushError_;}
    bool WritesStarted() const {return writesStarted_;}
    const std::string& Stage() const {return stage_;}
    DWORD Offset() const {return offset_;}
    unsigned WrittenTracks() const {return writtenTracks_;}
    unsigned VerifiedTracks() const {return verifiedTracks_;}
    unsigned PlannedTracks() const {return plannedTracks_;}
    const std::string& Diagnostics() const {return diagnostics_;}
private:
    void Mark(const char* stage,DWORD offset=0,DWORD requested=0);
    void Report(bool ok,int drive);
    FloppyWriteApi* api_;
    DWORD error_;
    DWORD flushError_=0;
    bool writesStarted_;
    std::string stage_,diagnostics_;
    DWORD offset_=0,requested_=0,transferred_=0;
    unsigned writtenTracks_=0,verifiedTracks_=0,plannedTracks_=160;
};
bool PrepareFloppyHfs(const char* name,std::vector<BYTE>& image,std::string& error);
bool LoadFloppyHfs(const char* path,std::vector<BYTE>& image,std::string& error);
bool ValidateFloppyHfs(const std::vector<BYTE>& image);
