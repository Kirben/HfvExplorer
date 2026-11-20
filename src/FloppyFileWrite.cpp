// SPDX-License-Identifier: GPL-2.0-or-later
#include "FloppyFileWrite.h"
#include "MediaWriteBackend.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"

namespace {
[[noreturn]] void ReportWriteFailure(const DirectFloppy& disk) {
 throw Storage::MediaIoError(disk.Error(),"Floppy write or verification failed (Windows error "+
     std::to_string(disk.Error())+"). The disk may be incomplete");
}
}
void RunFloppyWrite(int drive,std::optional<DWORD> generation,const std::function<void()>& mutate)
{
 physical_floppy_suspend(drive);
 FloppyWriteApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
 api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));
 if(!api)throw Storage::MediaIoError(ERROR_NOT_SUPPORTED,"No simulated writable floppy is attached");
#endif
 DirectFloppy disk(api);
 if(!disk.Open(drive,true))
  throw Storage::MediaIoError(disk.Error(),"Cannot open and lock the floppy");
 if(generation && disk.Generation()!=*generation)
  throw Storage::MediaIoError(ERROR_MEDIA_CHANGED,"Media changed after selection; select the volume again");
 try {
  mutate();
 } catch (...) {
  if(disk.Error())ReportWriteFailure(disk);
  throw;
 }
 if(!disk.Finish())ReportWriteFailure(disk);
}
