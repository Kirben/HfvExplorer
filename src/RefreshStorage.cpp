// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BackgroundCopies.h"
#include "VolumeCoordinator.h"
#include "RefreshStorage.h"
#include "MediaDiscovery.h"
#include "StorageJobDialog.h"
extern "C" {
#include "floppy.h"
}
namespace {
using ScanProbe=std::function<bool(int,bool,Storage::Job&)>;
std::function<Storage::Result(Storage::Job&)> ScanJob(bool floppies,bool cds,std::shared_ptr<std::vector<PhysicalVolume>> output,ScanProbe probe) {
 return [=](Storage::Job& job)->Storage::Result {
  Storage::Progress progress;progress.phase="Checking removable media";job.progress(progress);
  std::vector<PhysicalVolume> found;
  for(int drive=0;drive<26;++drive) {
   std::unique_ptr<VolumeCoord::OperationLease> lease;try{lease.reset(new VolumeCoord::OperationLease({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}}));}catch(const VolumeCoord::Busy&){continue;}
   if(job.cancelled())throw Storage::Cancelled();
   progress.path=std::string(1,char('A'+drive))+":";
   job.progress(progress);
   if(floppies&&probe(drive,true,job))found.push_back({drive,true});
   if(cds&&drive>=2&&probe(drive,false,job))found.push_back({drive,false});
  }
  *output=std::move(found);return {};
 };
}
}
std::function<Storage::Result(Storage::Job&)> MakeMediaScanJob(bool floppies,bool cds,
    std::shared_ptr<std::vector<PhysicalVolume>> output,std::function<bool(int,bool)> probe) {
 return ScanJob(floppies,cds,std::move(output),[probe=std::move(probe)](int drive,bool floppy,Storage::Job&){
  return probe(drive,floppy);
 });
}
std::vector<PhysicalVolume> ScanPhysicalMedia(bool floppies,bool cds) {
#ifdef HFV_TESTING
 return {}; // Only explicitly injected probes may access media in test builds.
#endif
 auto output=std::make_shared<std::vector<PhysicalVolume>>();
 auto result=ShowStorageWaitJob(ScanJob(floppies,cds,output,[](int drive,bool floppy,Storage::Job& job) {
  return floppy?MediaDiscovery::Floppy(drive,job):bool(MediaDiscovery::Optical(drive,job));
 }));
 if(result.code)AfxMessageBox(result.message.empty()?"Could not scan removable media.":result.message.c_str(),MB_OK|MB_ICONERROR);
 return *output;
}
