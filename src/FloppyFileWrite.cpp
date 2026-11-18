// SPDX-License-Identifier: GPL-2.0-or-later
#include "FloppyFileWrite.h"
#include "DirectFloppy.h"
#include "ReadOnlyCdBackend.h"

extern "C" void HFSIFACE_get_dir_flush(void);

namespace {
[[noreturn]] void ReportWriteFailure(const DirectFloppy& disk) {
 throw FloppyWriteFailure(disk.Error(),"Floppy write or verification failed (Windows error "+
     std::to_string(disk.Error())+"). The disk may be incomplete");
}
}
bool RunFloppyWrite(int drive,std::optional<DWORD> generation,const std::function<void()>& mutate)
{
 HFSIFACE_get_dir_flush();
 physical_floppy_suspend(drive);
 FloppyWriteApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
 api=dynamic_cast<FloppyWriteApi*>(physical_floppy_test_api(drive));
 if(!api)throw FloppyWriteFailure(ERROR_NOT_SUPPORTED,"No simulated writable floppy is attached");
#endif
 DirectFloppy disk(api);
 if(!disk.Open(drive,true))
  throw FloppyWriteFailure(disk.Error(),"Cannot open and lock the floppy");
 if(generation && disk.Generation()!=*generation)
  throw FloppyWriteFailure(ERROR_MEDIA_CHANGED,"Media changed after selection; select the volume again");
 try {
  mutate();
 } catch (...) {
  if(disk.Error())ReportWriteFailure(disk);
  throw;
 }
 HFSIFACE_get_dir_flush();
 if(!disk.Finish())ReportWriteFailure(disk);
 return disk.WritesStarted();
}
