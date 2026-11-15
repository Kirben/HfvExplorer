// SPDX-License-Identifier: GPL-2.0-or-later
#include "FloppyWriter.h"
#include "FakeFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "floppy.h"
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
static void check(bool ok,const char* action) {if(!ok){fprintf(stderr,"FAIL: %s (Windows=%lu HFS=%s)\n",action,GetLastError(),hfs_error?hfs_error:"");exit(1);}}
class FakeWriter : public FloppyWriteApi {
public:
 FakeFloppy fake;DWORD flushOverride=0;int failure=0;bool locked=false,ordered=true,flags=true;unsigned writes=0,formats=0,flushes=0,unlocks=0;
 explicit FakeWriter(const std::vector<BYTE>& media):fake(media) {}
 UINT DriveType(int d) override {return fake.DriveType(d);}
 HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f) override {
  flags=a==(GENERIC_READ|GENERIC_WRITE)&&s==(FILE_SHARE_READ|FILE_SHARE_WRITE)&&f==(FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH);
  if(failure==1) fake.openError=ERROR_ACCESS_DENIED;return fake.Open(p,a,s,f);
 }
 bool Geometry(HANDLE h,DISK_GEOMETRY& g) override {bool ok=fake.Geometry(h,g);if(failure==2)g.MediaType=RemovableMedia;return ok;}
 bool Verify(HANDLE h,DWORD& c) override {if(failure==12 && writes==11){SetLastError(ERROR_NOT_READY);return false;}return fake.Verify(h,c);}
 bool Read(HANDLE h,ULONGLONG o,DWORD n,void* p,DWORD& got) override {if(failure==14 && o==11*9216){got=0;SetLastError(ERROR_NOT_READY);return false;}bool ok=fake.Read(h,o,n,p,got);if((failure==8 || failure==16) && ok)static_cast<BYTE*>(p)[0]^=1;return ok;}
 void Close(HANDLE h) override {fake.Close(h);locked=false;}
 bool Lock(HANDLE) override {if(failure==3){SetLastError(ERROR_ACCESS_DENIED);return false;}locked=true;return true;}
 bool Dismount(HANDLE) override {ordered&=locked;if(failure==4){SetLastError(ERROR_ACCESS_DENIED);return false;}return true;}
 bool Unlock(HANDLE) override {++unlocks;locked=false;if(failure==11){SetLastError(ERROR_ACCESS_DENIED);return false;}return true;}
 bool FormatTracks1440(HANDLE) override {
  ordered&=locked;++formats;if(failure==10){SetLastError(ERROR_NOT_SUPPORTED);return false;}
  std::fill(fake.media.begin(),fake.media.end(),BYTE(0));++fake.changeCount;return true;
 }
 bool Write(HANDLE,ULONGLONG o,DWORD n,const void* p,DWORD& wrote) override {
  ordered&=locked && o%512==0 && n%512==0 && uintptr_t(p)%512==0;++writes;
  if(failure==13 && writes==12){wrote=0;SetLastError(ERROR_NOT_READY);return false;}
  if(failure==5){SetLastError(ERROR_WRITE_PROTECT);return false;}
  wrote=failure==6?n-1:n;memcpy(fake.media.data()+size_t(o),p,wrote);
  if(failure==9) ++fake.changeCount;return true;
 }
 bool Flush(HANDLE) override {++flushes;if(flushOverride){SetLastError(flushOverride);return false;}if(failure==7 || failure==15 || failure==16){SetLastError(failure==7?ERROR_WRITE_FAULT:ERROR_NOT_READY);return false;}return true;}
};
static void progress(const char* phase,unsigned completed,unsigned total,void*) {
 if(!completed || completed%20==0 || completed==total) {printf("%s %u/%u\n",phase,completed,total);fflush(stdout);}
}
static void save(const std::string& path,const std::vector<BYTE>& bytes) {
 HANDLE f=CreateFileA(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);check(f!=INVALID_HANDLE_VALUE,"create exclusive backup/output");
 DWORD wrote;check(WriteFile(f,bytes.data(),DWORD(bytes.size()),&wrote,NULL)&&wrote==bytes.size()&&FlushFileBuffers(f),"persist complete backup/output");CloseHandle(f);
}
static void hardware(const char* driveArg,const char* input,const char* directory) {
 check(!_stricmp(driveArg,"A:"),"hardware test is restricted to explicitly offered A:");
 std::vector<BYTE> populated,empty;std::string error;
 check(LoadFloppyHfs(input,populated,error)&&PrepareFloppyHfs("HFVFloppy",empty,error),"prepare and validate both HFS images before target writes");
 check(CreateDirectoryA(directory,NULL)!=FALSE,"new hardware test output directory");
 ReadOnlyFloppy reader;check(reader.OpenDrive(0)&&reader.MediaSize()==1474560,"read-only A: preflight");
 std::vector<BYTE> original(1474560);check(reader.ReadBytes(0,DWORD(original.size()),original.data()),"read every original A: sector before writing");reader.Close();
 save(std::string(directory)+"\\A-before.dsk",original);puts("Original A: sectors backed up. Initializing HFS...");fflush(stdout);
 FloppyWriter writer;
 if(!writer.WriteImage(0,empty,false,progress)) {fprintf(stderr,"HFS initialization failed: Windows %lu; writes started=%d\n",writer.Error(),writer.WritesStarted());exit(1);}
 puts("PASS: physical HFS initialization and full readback. Writing populated image...");fflush(stdout);
 if(!writer.WriteImage(0,populated,false,progress)) {fprintf(stderr,"Image write failed: Windows %lu; writes started=%d\n",writer.Error(),writer.WritesStarted());exit(1);}
 reader.Close();check(reader.OpenDrive(0)&&reader.FindHfs(),"reopen newly written physical HFS floppy");
 std::vector<BYTE> actual(1474560);check(reader.ReadBytes(0,DWORD(actual.size()),actual.data())&&actual==populated,"fresh-handle full-disc byte comparison");reader.Close();
 floppy_module_global_init();char path[64];get_floppy_volume_file_name(0,path);hfsvol* volume=hfs_mount(path,0,O_RDONLY);check(volume!=NULL,"mount actual written HFS filesystem");
 char name[]="FloppyTest:Payload";hfsfile* file=hfs_open(volume,name);check(file!=NULL,"open physical test file");BYTE data[8193];
 check(hfs_read(file,data,sizeof(data))==sizeof(data),"read physical data fork");for(unsigned i=0;i<sizeof(data);++i)check(data[i]==BYTE(i*37),"physical data fork fidelity");
 check(hfs_setfork(file,1)==0&&hfs_read(file,data,1025)==1025,"read physical resource fork");for(unsigned i=0;i<1025;++i)check(data[i]==BYTE(i*71),"physical resource fork fidelity");
 hfs_close(file);hfs_umount(volume);floppy_module_global_final();
 save(std::string(directory)+"\\A-verified.dsk",actual);
 puts("PASS: A: HFS format, image write, fresh-handle full readback and production HFS data/resource extraction. Low-level track formatting was not requested.");
}
int main(int argc,char** argv) {
 if(argc==5 && !strcmp(argv[1],"--hardware")) {hardware(argv[2],argv[3],argv[4]);return 0;}
 check(argc==1,"Usage: floppy-write [--hardware A: POPULATED_TEST_IMAGE NEW_OUTPUT_DIRECTORY]");
 std::vector<BYTE> image;std::string error;check(PrepareFloppyHfs("WriteTest",image,error),"prepare 1.44 MB HFS format image");
 for(bool tracks:{false,true}) {
  FakeWriter api(std::vector<BYTE>(1474560,0xa5));FloppyWriter writer(&api);
  check(writer.WriteImage(25,image,tracks)&&api.fake.media==image&&api.flags&&api.ordered&&api.writes==160&&api.flushes==1&&api.fake.opens==api.fake.closes,"aligned locked image write and full readback");
  check(api.formats==(tracks?1:0),"track format is explicitly optional");
 }
 for(int fail=1;fail<=11;++fail) {
  std::vector<BYTE> original(1474560,0xa5);FakeWriter api(original);api.failure=fail;FloppyWriter writer(&api);
  check(!writer.WriteImage(0,image,fail==10)&&writer.Error()!=0,"operation failures are reported");
  check(api.fake.closes==api.fake.opens-(fail==1?1:0)&&api.ordered,"failure closes/unlocks target");
  if(fail<=4) check(!writer.WritesStarted()&&api.writes==0&&api.fake.media==original,"validation/locking failure issues no target writes");
 }
 {FakeWriter api(std::vector<BYTE>(1474560));api.fake.driveType=DRIVE_FIXED;FloppyWriter writer(&api);check(!writer.WriteImage(0,image)&&api.fake.opens==0,"fixed disks cannot be targets");}
 {FakeWriter api(std::vector<BYTE>(1474560));FloppyWriter writer(&api);auto invalid=image;invalid.pop_back();check(!writer.WriteImage(0,invalid)&&api.fake.opens==0,"wrong image size rejected before device access");invalid=image;invalid[1024]=0;check(!writer.WriteImage(0,invalid)&&api.fake.opens==0,"non-HFS image rejected before access");}
 for(int fail=12;fail<=14;++fail) {
  FakeWriter api(std::vector<BYTE>(1474560));api.failure=fail;FloppyWriter writer(&api);
  check(!writer.WriteImage(0,image)&&writer.Error()==ERROR_NOT_READY,"preserve not-ready error after cleanup");
  check(writer.Offset()==101376 && writer.WrittenTracks()==(fail==14?160:11) && writer.VerifiedTracks()==(fail==14?11:0),"failure after 11 tracks records exact offset and counts");
  const char* stage=fail==12?"Media readiness before write":fail==13?"WriteFile track transfer":"ReadFile readback transfer";
  check(writer.Stage()==stage && writer.Diagnostics().find("sector=198 cylinder=5 head=1")!=std::string::npos,"distinguish readiness, write and readback failures with sector/CHS report");
  check(api.fake.opens==api.fake.closes && api.unlocks==1,"diagnostics preserve failure cleanup");
 }
 {FakeWriter api(std::vector<BYTE>(1474560));api.failure=15;FloppyWriter writer(&api);
  check(!writer.WriteImage(0,image) && writer.Error()==ERROR_NOT_READY && writer.FlushError()==ERROR_NOT_READY,"flush failure remains visible after readback");
  check(writer.WrittenTracks()==160 && writer.VerifiedTracks()==160 && writer.Stage()=="Flush written data (all readbacks matched)","flush failure does not skip full readback or become success");
  check(api.fake.opens==api.fake.closes && api.unlocks==1,"flush warning preserves cleanup");}
 {FakeWriter api(std::vector<BYTE>(1474560));api.failure=16;FloppyWriter writer(&api);
  check(!writer.WriteImage(0,image) && writer.Error()==ERROR_CRC && writer.FlushError()==ERROR_NOT_READY && writer.VerifiedTracks()==0,"readback corruption remains an error after a flush failure");
  check(writer.Stage()=="Compare readback" && writer.Diagnostics().find("Flush failed: error=21")!=std::string::npos,"retain both flush and subsequent verification failures");}
 for(DWORD unsupported:{DWORD(ERROR_INVALID_FUNCTION),DWORD(ERROR_NOT_SUPPORTED)}) {
  FakeWriter api(std::vector<BYTE>(1474560));api.flushOverride=unsupported;FloppyWriter writer(&api);
  check(writer.WriteImage(0,image) && writer.Error()==0 && writer.FlushError()==unsupported && writer.WrittenTracks()==160 && writer.VerifiedTracks()==160,"unsupported flush succeeds only with complete write and verification");
  check(api.fake.opens==api.fake.closes && api.unlocks==1 && writer.Diagnostics().find("RESULT=PASS")!=std::string::npos,"unsupported flush success logs and cleans up");
  api.failure=8;check(!writer.WriteImage(0,image) && writer.Error()==ERROR_CRC,"unsupported flush cannot hide readback corruption");
  api.failure=14;check(!writer.WriteImage(0,image) && writer.Error()==ERROR_NOT_READY,"unsupported flush cannot hide readback I/O failure");
  api.failure=11;check(!writer.WriteImage(0,image) && writer.Error()==ERROR_ACCESS_DENIED,"unsupported flush cannot hide unlock failure");
 }
 for(DWORD realError:{DWORD(ERROR_NOT_READY),DWORD(ERROR_ACCESS_DENIED),DWORD(ERROR_WRITE_FAULT)}) {
  FakeWriter api(std::vector<BYTE>(1474560));api.flushOverride=realError;FloppyWriter writer(&api);
  check(!writer.WriteImage(0,image) && writer.Error()==realError && writer.VerifiedTracks()==160,"other flush errors remain failures even after matching readback");
 } puts("PASS: 1.44 MB HFS preparation, optional track format, locked aligned image writes, flush/readback, write protection, short writes, corruption, media change, target/size guards, cleanup and exact failure diagnostics; all writes simulated");return 0;
}
