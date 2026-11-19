// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "CdImage.h"
#include <array>

namespace CdMedia {
constexpr DWORD Form2Bytes=2324;

enum class RawOutput { BinCue, RawTrack, Form2Track, WaveTracks };
enum class SectorMode { Unknown, Mode1, Mode2 };
SectorMode RawSectorMode(const BYTE*);
bool IsForm2(const BYTE*);
std::array<BYTE,44> WaveHeader(DWORD dataBytes);

class RawSource {
public:
    virtual ~RawSource()=default;
    virtual bool Open(int drive)=0;
    virtual void Close()=0;
    virtual std::vector<Track> Tracks()const=0;
    virtual bool Verify(DWORD& generation)=0;
    virtual bool Read(const Track&,uint64_t sector,unsigned count,void*,DWORD& returned)=0;
    virtual DWORD Error()const=0;
};
std::shared_ptr<RawSource> NativeRawSource();

struct RawSelection {
    RawOutput format;
    std::vector<Track> tracks;
    DWORD generation;
};
std::function<Storage::Result(Storage::Job&)> InspectRawCd(int,std::shared_ptr<RawSource>,
                                  std::shared_ptr<std::vector<Track>>,std::shared_ptr<DWORD>);
std::function<Storage::Result(Storage::Job&)> SaveRawCd(int,std::wstring,RawSelection,
                                                       std::shared_ptr<RawSource>);
}
