// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "FloppyImage.h"
#include "ReadOnlyFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "CdImage.h"
#include "StorageJobDialog.h"
namespace {
class FloppyImageSource : public MediaImageSource {
 FloppyReadApi* api_;ReadOnlyFloppy reader_;DWORD error_=0;
public:
 explicit FloppyImageSource(FloppyReadApi* api):api_(api),reader_(api){}
 bool Open(int drive,std::string& reason) override {
  if(!api_){error_=ERROR_NOT_SUPPORTED;reason="No simulated floppy was attached";return false;}
  if(!reader_.OpenDrive(drive)){error_=reader_.Error();reason="Could not open a supported sector-formatted floppy";return false;}return true;
 }
 void Close()override{reader_.Close();}
 unsigned long long Size() const override{return reader_.MediaSize();}
 bool Verify(DWORD& generation) override {if(!api_->Verify(reader_.Handle(),generation)){error_=GetLastError();return false;}return true;}
 bool Read(unsigned long long offset,DWORD count,void* bytes,DWORD& got) override {
  got=0;if(!reader_.ReadBytes(offset,count,bytes)){error_=reader_.Error();return false;}got=count;return true;
 }
 DWORD Error() const override{return error_;}
};
}
std::function<Storage::Result(Storage::Job&)> MakeFloppyImageJob(int drive,std::string output,FloppyReadApi* api){
#ifdef HFV_FLOPPY_TESTING
 if(!api)api=physical_floppy_test_api(drive);
#else
 if(!api)api=&NativeFloppyReadApi();
#endif
 return MakeMediaImageJob(drive,std::move(output),std::make_shared<FloppyImageSource>(api),"Floppy");
}
