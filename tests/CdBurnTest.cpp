// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "resource.h"
#include "DeviceWait.h"
#include "CdBurn.h"
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
#include "OptionsSheet.h"
#include <imapi2error.h>
#include <stdio.h>
#include <stdexcept>
extern bool ProbeCdBurnLayout();
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void save(const std::string& path,const std::vector<BYTE>& bytes){CFile f;check(f.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create source fixture");if(!bytes.empty())f.Write(bytes.data(),UINT(bytes.size()));}
std::vector<BYTE> load(const std::string& path){CFile f;check(f.Open(path.c_str(),CFile::modeRead)!=FALSE,"read source fixture");std::vector<BYTE> b(size_t(f.GetLength()));check(f.Read(b.data(),UINT(b.size()))==b.size(),"read source bytes");return b;}
class FakeBurn : public CdBurnBackend {
public:
 CdBurnMedia media={true,true,1000};unsigned opens=0,inspections=0,configurations=0,writes=0;HRESULT openResult=S_OK,configureResult=S_OK,writeResult=S_OK;
 DWORD delay=0;std::vector<BYTE> received;std::string path;bool sourceLocked=false;
 HRESULT Open(const std::wstring&) override{++opens;return openResult;}
 HRESULT Inspect(CdBurnMedia& out) override{++inspections;out=media;return S_OK;}
 HRESULT ConfigureVerifiedFinalizedWrite() override{++configurations;return configureResult;}
 HRESULT Write(IStream* stream,Storage::Job& job) override {
  ++writes;check(configurations>0,"burn without verification/finalization configuration");
  HANDLE writer=CreateFileA(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  sourceLocked=writer==INVALID_HANDLE_VALUE;if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
  STATSTG info={};check(SUCCEEDED(stream->Stat(&info,STATFLAG_NONAME)),"source stream stat");
  BYTE bytes[4096];ULONG got=0;
  for(;;){
   if(job.cancelled())return E_IMAPI_REQUEST_CANCELLED;
   HRESULT hr=stream->Read(bytes,sizeof(bytes),&got);if(FAILED(hr))return hr;if(!got)break;
   received.insert(received.end(),bytes,bytes+got);auto p=job.progress();p.phase="Burning image";p.scanning=false;p.byteTransfer=true;p.totalBytes=info.cbSize.QuadPart;p.processed=received.size();job.progress(p);
   if(delay)Sleep(delay);
  }
  auto p=job.progress();p.phase="Verifying burned disc";p.scanning=true;job.progress(p);return writeResult;
 }
};
}
int RunCdBurnTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory,path=root+"\\source.iso";
 FILE* report=nullptr;if(fopen_s(&report,(root+"\\cd-burn-report.txt").c_str(),"w")||!report)return 2;
 try {
  check(ProbeCdBurnLayout(),"Burn CD layout, default recorder and preparation requirements");
  std::vector<BYTE> bytes(32768);for(size_t n=0;n<bytes.size();++n)bytes[n]=BYTE(n*37);save(path,bytes);
  auto prepare=[&](std::shared_ptr<FakeBurn> burner,std::shared_ptr<CdBurnPlan> plan,const std::string& image){auto job=Storage::worker().submit(PrepareCdBurn(image,L"simulated-recorder",plan,burner));job->wait();return job->result();};
  auto execute=[&](std::shared_ptr<FakeBurn> burner,const CdBurnPlan& plan){burner->path=path;auto job=Storage::worker().submit(MakeCdBurnJob(plan,burner));job->wait();return job;};
  auto burner=std::make_shared<FakeBurn>();auto plan=std::make_shared<CdBurnPlan>();
  check(prepare(burner,plan,path).code==0&&plan->bytes==bytes.size()&&burner->writes==0&&burner->configurations==0,"preflight is read-only and returns image identity/size");
  auto completed=execute(burner,*plan);
  check(completed->result().code==0&&burner->received==bytes&&burner->sourceLocked&&burner->writes==1&&burner->opens==2&&burner->inspections==2,"exact raw image stream, source locked and media checked again before recording");
  check(completed->progress().processed==bytes.size()&&load(path)==bytes,"completed verified burn progress and unchanged source");
  for(int failure=0;failure<4;++failure){
   auto bad=std::make_shared<FakeBurn>();auto output=std::make_shared<CdBurnPlan>();
   if(failure==0)bad->media.blank=false;if(failure==1)bad->media.supported=false;if(failure==2)bad->media.freeSectors=1;if(failure==3)bad->openResult=E_ACCESSDENIED;
   check(prepare(bad,output,path).code!=0&&bad->writes==0,"nonblank/unsupported/undersized/busy media fail without recording");
  }
  for(size_t length:{size_t(0),size_t(2049)}){std::string invalid=root+"\\invalid.iso";save(invalid,std::vector<BYTE>(length));auto bad=std::make_shared<FakeBurn>();check(prepare(bad,std::make_shared<CdBurnPlan>(),invalid).code!=0&&bad->opens==0,"empty and unaligned images rejected before recorder access");}
  auto missing=std::make_shared<FakeBurn>();check(prepare(missing,std::make_shared<CdBurnPlan>(),root+"\\missing.iso").code!=0&&missing->opens==0,"missing source rejected before recorder access");
  {
   auto bad=std::make_shared<FakeBurn>();bad->media.blank=false;check(execute(bad,*plan)->result().code!=0&&bad->writes==0,"disc changed to nonblank after preflight rejected before recording");
  }
  {
   auto bad=std::make_shared<FakeBurn>();bad->configureResult=E_NOINTERFACE;check(execute(bad,*plan)->result().code!=0&&bad->writes==0,"verification unavailable fails before recording");
  }
  {
   auto bad=std::make_shared<FakeBurn>();bad->writeResult=E_IMAPI_BURN_VERIFICATION_FAILED;
   auto failed=execute(bad,*plan);check(failed->result().code!=0&&failed->result().message.find("incomplete")!=std::string::npos,"failed verification is not reported as successful burn");
  }
  {
   auto cancelled=Storage::worker().submit([&](Storage::Job& job){job.cancel();return MakeCdBurnJob(*plan,burner)(job);});cancelled->wait();check(cancelled->result().error==ERROR_CANCELLED&&burner->writes==1,"pre-record cancellation issues no new write");
  }
  {
   HANDLE change=CreateFileA(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);check(change!=INVALID_HANDLE_VALUE,"open image to simulate modification");
   ULARGE_INTEGER time;time.LowPart=plan->identity.ftLastWriteTime.dwLowDateTime;time.HighPart=plan->identity.ftLastWriteTime.dwHighDateTime;time.QuadPart+=10000000;FILETIME changed={time.LowPart,time.HighPart};
   check(SetFileTime(change,nullptr,nullptr,&changed)!=FALSE,"change source identity");CloseHandle(change);
   auto bad=std::make_shared<FakeBurn>();check(execute(bad,*plan)->result().error==ERROR_FILE_INVALID&&bad->opens==0,"source modified after confirmation/preflight rejected before recorder access");
  }
  auto uiBurner=std::make_shared<FakeBurn>();plan=std::make_shared<CdBurnPlan>();check(prepare(uiBurner,plan,path).code==0,"prepare UI fixture");uiBurner->path=path;uiBurner->delay=60;
  CWnd owner;CString type=AfxRegisterWndClass(0);check(owner.CreateEx(0,type,"Hidden burn owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"create hidden burn owner");AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();
  storageUiTest={};auto success=ShowStorageJob(MakeCdBurnJob(*plan,uiBurner),"Hidden burn",true,true,true);
  check(success.code==0&&storageUiTest.timerTicks>=2&&storageUiTest.copyBarCount==1&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"burn progress stays responsive and restores owner");
  auto cancelBurner=std::make_shared<FakeBurn>();cancelBurner->path=path;cancelBurner->delay=100;storageUiTest={};storageUiTest.cancelOnTimer=true;
  auto uiCancel=ShowStorageJob(MakeCdBurnJob(*plan,cancelBurner),"Hidden cancel burn",true,true,true);
  check(uiCancel.error==ERROR_CANCELLED&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled()&&load(path)==bytes,"Cancel button stops simulated recording and restores owner/source");
  storageUiTest={};{
   StorageAction action;auto preflight=ShowStorageWaitJob(PrepareCdBurn(path,L"simulated-recorder",std::make_shared<CdBurnPlan>(),std::make_shared<FakeBurn>()),true);
   check(preflight.code==0&&!StorageUi::state().sessionOpen&&owner.IsWindowEnabled(),"complete preflight closes waiting dialog before configuration/confirmation UI");
  }
  auto modeless=std::make_shared<FakeBurn>();
  modeless->delay=60;
  const char verified[]="Recording finalized and verified.";
  storageUiTest={};
  check(BackgroundCopies::StartStorage(MakeCdBurnJob(*plan,modeless),&owner,"Hidden modeless burn",
        "CD burn",{},{true,verified})&&owner.IsWindowEnabled(),"submit modeless burn");
  const auto progress=BackgroundCopies::DialogWindow();
  const auto started=GetTickCount();
  while(BackgroundCopies::Active()&&GetTickCount()-started<5000){
   MSG message{};
   while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT)DispatchMessage(&message);}
   Sleep(1);
  }
  char detail[128]{};
  GetDlgItemTextA(progress,IDC_CP_FILE,detail,ARRAYSIZE(detail));
  check(!BackgroundCopies::Active()&&!BackgroundCopies::LastResult().code&&
        modeless->configurations==1&&modeless->writes==1&&!strcmp(detail,verified)&&
        !storageUiTest.disabledDuring&&owner.IsWindowEnabled()&&load(path)==bytes,
        "modeless burn failed to finalize, verify, report completion or preserve its input");
  SendMessage(progress,WM_CLOSE,0,0);
  COptionsSheet options("Options",nullptr,99);check(options.GetPageCount()==3&&options.m_last_active_page==2,"HPBurner page removed and remembered tab index clamped");
  check(FindResourceA(AfxGetResourceHandle(),MAKEINTRESOURCEA(167),RT_DIALOG)==nullptr,"HPBurner tab resource removed");
  owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(report,"PASS: read-only preflight, blank/support/capacity guards, exact source stream and source locking, revalidation, verification configuration/failure, cancellation before/during recording, progress and pre-close owner restoration, completed preflight UI lifecycle, HPBurner tab removal and page bounds; all recorder operations simulated.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}catch(CException* error){char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: %s\n",text);error->Delete();fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
