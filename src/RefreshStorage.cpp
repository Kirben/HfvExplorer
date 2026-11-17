// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BackgroundCopies.h"
#include "RefreshStorage.h"
#include "StorageJobDialog.h"
extern "C" {
#include "floppy.h"
}
std::function<Storage::Result(Storage::Job&)> MakeMediaScanJob(bool floppies,bool cds,std::shared_ptr<std::vector<PhysicalVolume>> output,std::function<bool(int,bool)> probe) {
 return [=](Storage::Job& job)->Storage::Result {
  Storage::Progress progress;progress.phase="Checking removable media";job.progress(progress);
  std::vector<PhysicalVolume> found;
  for(int drive=0;drive<26;++drive) {
   progress.path=std::string(1,char('A'+drive))+":";
   job.progress(progress);
   if(floppies&&probe(drive,true))found.push_back({drive,true});
   if(cds&&drive>=2&&probe(drive,false))found.push_back({drive,false});
  }
  *output=std::move(found);return {};
 };
}
std::vector<PhysicalVolume> ScanPhysicalMedia(bool floppies,bool cds) {
 if(BackgroundCopies::Active())return {};
 auto output=std::make_shared<std::vector<PhysicalVolume>>();
 auto result=ShowStorageWaitJob(MakeMediaScanJob(floppies,cds,output,[](int drive,bool floppy) {
  char path[]="A:\\";path[0]+=char(drive);
  if(floppy)return is_hfs_floppy_present(drive)!=0;
  return GetDriveTypeA(path)==DRIVE_CDROM&&is_hfs_cd_present(drive)!=0;
 }));
 if(result.code)AfxMessageBox(result.message.empty()?"Could not scan removable media.":result.message.c_str(),MB_OK|MB_ICONERROR);
 return *output;
}
