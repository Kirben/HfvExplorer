// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "CdImage.h"
#include "CdImageToc.h"
#include "StorageJobDialog.h"
#include <vector>
#include <stdio.h>
#include <stdexcept>
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
class FakeCdImage : public CdImageSource {
public:
 std::vector<BYTE> data;DWORD error=0,generation=7;unsigned reads=0,checks=0,opens=0,changeAtRead=0,changeAtCheck=0;
 DWORD readError=0,openError=0,delay=0;bool shortRead=false;std::function<void()> onRead;
 FakeCdImage():data(3*65536+2048){for(size_t n=0;n<data.size();++n)data[n]=BYTE(n*71+n/2048);data[0]='E';data[1]='R';data[512]='P';data[513]='M';}
 bool Open(int,std::string& why) override {++opens;if(openError){error=openError;why="Injected unsupported or unavailable CD";return false;}return true;}
 unsigned long long Size() const override{return data.size();}
 bool Verify(DWORD& value) override {++checks;if(changeAtCheck==checks)++generation;value=generation;return true;}
 bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got) override {
  ++reads;if(delay)Sleep(delay);got=0;
  if(onRead)onRead();if(readError){error=readError;return false;}
  if(offset>data.size()||count>data.size()-offset){error=ERROR_HANDLE_EOF;return false;}
  got=shortRead?count-1:count;memcpy(buffer,data.data()+size_t(offset),got);if(changeAtRead==reads)++generation;return true;
 }
 DWORD Error() const override{return error;}
};
void save(const std::string& path,const std::vector<BYTE>& data){CFile file;check(file.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create host fixture");file.Write(data.data(),UINT(data.size()));}
std::vector<BYTE> load(const std::string& path){CFile file;check(file.Open(path.c_str(),CFile::modeRead)!=FALSE,"open image");std::vector<BYTE> data(size_t(file.GetLength()));check(file.Read(data.data(),UINT(data.size()))==data.size(),"read image");return data;}
void noTemporary(const std::string& root){WIN32_FIND_DATAA found;HANDLE h=FindFirstFileA((root+"\\hcd*.tmp").c_str(),&found);if(h!=INVALID_HANDLE_VALUE)FindClose(h);check(h==INVALID_HANDLE_VALUE,"partial temporary image leaked");}
}
int RunCdImageTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;
 FILE* report=nullptr;if(fopen_s(&report,(root+"\\cd-image-report.txt").c_str(),"w")||!report)return 2;
 try {
  CDROM_TOC toc={};toc.FirstTrack=toc.LastTrack=1;toc.TrackData[0].TrackNumber=1;toc.TrackData[0].Control=4;toc.TrackData[1].TrackNumber=0xaa;toc.TrackData[1].Address[2]=1;
  check(CdImageSupportedToc(toc,sizeof(toc))&&CdImageTocBytes(toc)==256*2048,"single data track and big-endian lead-out length");
  auto invalid=toc;invalid.TrackData[0].Control=0;check(!CdImageSupportedToc(invalid,sizeof(invalid)),"audio CD rejected");
  invalid=toc;invalid.LastTrack=2;check(!CdImageSupportedToc(invalid,sizeof(invalid)),"mixed/multitrack/multisession CD rejected");
  invalid=toc;invalid.TrackData[0].Address[3]=1;check(!CdImageSupportedToc(invalid,sizeof(invalid)),"nonzero first-track address rejected");
  invalid=toc;invalid.TrackData[1].TrackNumber=2;check(!CdImageSupportedToc(invalid,sizeof(invalid))&&!CdImageSupportedToc(toc,4),"missing lead-out and truncated TOCs rejected");
  toc.TrackData[1].Address[0]=1;check(CdImageTocBytes(toc)>0xffffffffULL,"TOC length does not truncate above 4 GiB");
  auto run=[&](const std::string& path,std::shared_ptr<FakeCdImage> source,Storage::ConflictChoice choice=Storage::ConflictChoice::Replace){
   auto job=Storage::worker().submit(MakeCdImageJob(3,path,source));DWORD start=GetTickCount();
   while(!job->done()){if(!job->conflict().empty())job->resolve(choice);if(GetTickCount()-start>10000){job->cancel();throw std::runtime_error("CD image job timed out");}Sleep(1);}
   noTemporary(root);return job;
  };
  std::string image=root+"\\disc.iso";auto source=std::make_shared<FakeCdImage>();
  auto success=run(image,source);
  check(success->result().code==0&&load(image)==source->data&&source->reads==4&&source->checks==10,"dump preserves all sectors/partition-map bytes with before/after/final generation checks");
  check(success->progress().processed==source->data.size()&&success->progress().totalBytes==source->data.size()&&success->progress().byteTransfer,"whole-image byte progress reaches exact length");
  const std::vector<BYTE> keep={'K','E','E','P'};
  for(int test=0;test<5;++test){
   save(image,keep);auto bad=std::make_shared<FakeCdImage>();
   if(test==0)bad->readError=ERROR_CRC;if(test==1)bad->shortRead=true;if(test==2)bad->changeAtRead=2;if(test==3)bad->changeAtCheck=10;if(test==4)bad->openError=ERROR_NOT_SUPPORTED;
   auto failed=run(image,bad);check(failed->result().code!=0&&!failed->result().message.empty()&&load(image)==keep,"read errors, short reads, media changes and unsupported media preserve existing image");
   if(test==0)check(failed->result().error==ERROR_CRC&&failed->result().message.find("byte offset 0")!=std::string::npos,"read failure records offset and error");
   if(test==2||test==3)check(failed->result().error==ERROR_MEDIA_CHANGED,"media changes are not mistaken for success");
  }
  for(auto choice:{Storage::ConflictChoice::Skip,Storage::ConflictChoice::Cancel}){
   auto untouched=std::make_shared<FakeCdImage>();auto job=run(image,untouched,choice);
   check(untouched->opens==0&&load(image)==keep,"skip/cancel preserve destination without opening CD");
   check(choice==Storage::ConflictChoice::Skip?job->result().code==0:job->result().error==ERROR_CANCELLED,"overwrite choice delivered");
  }
  auto replaced=std::make_shared<FakeCdImage>();check(run(image,replaced)->result().code==0&&load(image)==replaced->data,"explicit replacement publishes complete image");
  std::string race=root+"\\race.iso";auto racing=std::make_shared<FakeCdImage>();racing->onRead=[&]{if(racing->reads==1)save(race,keep);};
  check(run(race,racing)->result().code!=0&&load(race)==keep,"newly appearing destination is not overwritten");racing->onRead=nullptr;
  auto missing=std::make_shared<FakeCdImage>();check(run(root+"\\missing\\disc.iso",missing)->result().code!=0,"destination creation errors reported");
  auto cancelling=std::make_shared<FakeCdImage>();
  auto cancelled=Storage::worker().submit([&](Storage::Job& job){cancelling->onRead=[&job]{job.cancel();};return MakeCdImageJob(3,root+"\\cancelled.iso",cancelling)(job);});cancelled->wait();cancelling->onRead=nullptr;
  check(cancelled->result().error==ERROR_CANCELLED&&GetFileAttributesA((root+"\\cancelled.iso").c_str())==INVALID_FILE_ATTRIBUTES,"mid-read cancellation discards partial image");noTemporary(root);
  CWnd owner;CString ownerClass=AfxRegisterWndClass(0);check(owner.CreateEx(0,ownerClass,"Hidden CD image owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"create hidden image owner");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();storageUiTest={};
  auto slow=std::make_shared<FakeCdImage>();slow->delay=80;
  auto ui=ShowStorageJob(MakeCdImageJob(3,root+"\\ui.iso",slow),"Hidden CD image",true,true,true);
  check(ui.code==0&&storageUiTest.dialogCreations==1&&storageUiTest.copyBarCount==1&&storageUiTest.timerTicks>=2&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"slow image dump uses one responsive progress dialog and restores owner");
  storageUiTest={};storageUiTest.cancelOnTimer=true;auto uiCancelSource=std::make_shared<FakeCdImage>();uiCancelSource->delay=100;
  auto uiCancel=ShowStorageJob(MakeCdImageJob(3,root+"\\ui-cancel.iso",uiCancelSource),"Hidden cancel CD image",true,true,true);
  check(uiCancel.error==ERROR_CANCELLED&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled()&&GetFileAttributesA((root+"\\ui-cancel.iso").c_str())==INVALID_FILE_ATTRIBUTES,"Cancel button cancels between reads, removes partial image and restores owner");noTemporary(root);
  owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(report,"PASS: complete-sector CD image dump, partition-map byte fidelity, exact progress, read/short-read errors, media changes including final check, unsupported media, overwrite/skip/cancel, replacement publication, destination race/errors, partial cleanup, slow responsive dialog, Cancel button and pre-close owner restoration; all CD sources simulated.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}catch(CException* error){char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: %s\n",text);error->Delete();fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
