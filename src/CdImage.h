// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include "CdImageToc.h"
class MediaImageSource {
public:
 virtual ~MediaImageSource(){}
 virtual bool Open(int drive,std::string& reason)=0;
 virtual void Close(){}
 virtual unsigned long long Size() const=0;
 virtual bool Verify(DWORD& generation)=0;
 virtual bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got)=0;
 virtual DWORD Error() const=0;
 virtual std::vector<CdMedia::Track> DataTracks()const{CdMedia::Track t;t.sectors=Size()/2048;return {t};}
 virtual bool SelectTrack(unsigned number,std::string& reason){if(number==1)return true;reason="Data track is not available";return false;}
};
using CdImageSource=MediaImageSource;
std::shared_ptr<CdImageSource> MakeNativeCdImageSource();
struct CdImageSelection {unsigned track=0;bool identityKnown=false,macVolumeOnly=false;DWORD generation=0;CdMedia::Track expected;};
std::function<Storage::Result(Storage::Job&)> MakeCdTrackInspectionJob(int drive,std::shared_ptr<CdImageSource>,std::shared_ptr<std::vector<CdMedia::Track>>,std::shared_ptr<DWORD> generation);
std::function<Storage::Result(Storage::Job&)> MakeMediaImageJob(int drive,std::string output,std::shared_ptr<MediaImageSource> source,std::string medium);
std::function<Storage::Result(Storage::Job&)> MakeCdImageJob(int drive,std::string output,std::shared_ptr<CdImageSource> source=nullptr,CdImageSelection selection={});
