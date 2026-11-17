// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "CdBurn.h"
#include <atlbase.h>
#include <atlconv.h>
#include <imapi2.h>
#include <imapi2error.h>
#include <ocidl.h>
#include <shlwapi.h>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"shlwapi.lib")
std::vector<std::string> CdBurnBackend::Resources(const std::wstring& recorder){int n=WideCharToMultiByte(CP_UTF8,0,recorder.data(),int(recorder.size()),nullptr,0,nullptr,nullptr);std::string id(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,recorder.data(),int(recorder.size()),&id[0],n,nullptr,nullptr);return {"recorder:"+id};}
namespace {
struct Apartment {HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(result))CoUninitialize();}};
Storage::Result error(HRESULT hr,const char* phase,bool recording=false){
 char text[512];sprintf_s(text,"%s failed (HRESULT 0x%08lX).%s",phase,ULONG(hr),recording?" Recording was requested; the disc may be incomplete.":"");
 return {1,hr==E_IMAPI_REQUEST_CANCELLED?ERROR_CANCELLED:DWORD(hr),text};
}
void publish(Storage::Job& job,const char* phase,unsigned long long done=0,unsigned long long total=0){
 auto p=job.progress();p.phase=phase;p.scanning=!total;p.byteTransfer=total!=0;p.totalBytes=total;p.processed=min(done,total);job.progress(p);
}
// Invoked by IMAPI on the burning thread. It only updates the job snapshot.
class BurnEvents : public DDiscFormat2DataEvents {
 LONG references_=1;Storage::Job& job_;IDiscFormat2Data* writer_;
public:
 BurnEvents(Storage::Job& job,IDiscFormat2Data* writer):job_(job),writer_(writer){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown||iid==IID_IDispatch||iid==__uuidof(DDiscFormat2DataEvents)){*out=static_cast<DDiscFormat2DataEvents*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&references_);}
 ULONG STDMETHODCALLTYPE Release() override{LONG n=InterlockedDecrement(&references_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override{if(!n)return E_POINTER;*n=0;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*) override{return DISP_E_UNKNOWNNAME;}
 HRESULT STDMETHODCALLTYPE Invoke(DISPID id,REFIID,LCID,WORD,DISPPARAMS* args,VARIANT*,EXCEPINFO*,UINT*) override {
  if(id!=DISPID_DDISCFORMAT2DATAEVENTS_UPDATE)return DISP_E_MEMBERNOTFOUND;
  if(!args||args->cArgs!=2||args->rgvarg[0].vt!=VT_DISPATCH||!args->rgvarg[0].pdispVal)return DISP_E_BADPARAMCOUNT;
  return Update(nullptr,args->rgvarg[0].pdispVal);
 }
 HRESULT STDMETHODCALLTYPE Update(IDispatch*,IDispatch* progress) override {
  try{return update(progress);}catch(...){writer_->CancelWrite();return E_OUTOFMEMORY;}
 }
 HRESULT update(IDispatch* progress) {
  if(!progress)return E_POINTER;
  if(job_.cancelled())writer_->CancelWrite();
  CComPtr<IDiscFormat2DataEventArgs> event;
  if(FAILED(progress->QueryInterface(IID_PPV_ARGS(&event))))return S_OK;
  IMAPI_FORMAT2_DATA_WRITE_ACTION action; if(FAILED(event->get_CurrentAction(&action)))return S_OK;
  const char* phase="Preparing recorder";
  switch(action){
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_VALIDATING_MEDIA:phase="Checking blank disc";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_FORMATTING_MEDIA:phase="Preparing disc";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_INITIALIZING_HARDWARE:phase="Initializing recorder";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_CALIBRATING_POWER:phase="Calibrating recorder";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_WRITING_DATA:{
    LONG first=0,last=0,total=0;
    if(SUCCEEDED(event->get_StartLba(&first))&&SUCCEEDED(event->get_LastWrittenLba(&last))&&SUCCEEDED(event->get_SectorCount(&total))&&total>0){
     auto sectors=last>=first?min(static_cast<unsigned long long>(LONGLONG(last)-first+1),static_cast<unsigned long long>(total)):0;
     publish(job_,"Burning image",sectors*2048,static_cast<unsigned long long>(total)*2048);return S_OK;
    }phase="Burning image";break;
   }
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_FINALIZATION:phase="Finalizing disc";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_VERIFYING:phase="Verifying burned disc";break;
   case IMAPI_FORMAT2_DATA_WRITE_ACTION_COMPLETED:phase="Completing burn";break;
  }
  publish(job_,phase);return S_OK;
 }
};
class NativeBurn : public CdBurnBackend {
 CComPtr<IDiscRecorder2> recorder_;CComPtr<IDiscFormat2Data> writer_;
 bool exclusive_=false;
public:
 ~NativeBurn(){Close();}
 void Close()override{if(exclusive_){recorder_->ReleaseExclusiveAccess();exclusive_=false;}writer_.Release();recorder_.Release();}
 std::vector<std::string> Resources(const std::wstring& id)override{
  auto keys=CdBurnBackend::Resources(id);CComPtr<IDiscRecorder2> recorder;HRESULT hr=recorder.CoCreateInstance(__uuidof(MsftDiscRecorder2));if(FAILED(hr))throw std::runtime_error("Cannot identify recorder resources");hr=recorder->InitializeDiscRecorder(CComBSTR(id.c_str()));if(FAILED(hr))throw std::runtime_error("Cannot identify recorder resources");SAFEARRAY* paths=nullptr;hr=recorder->get_VolumePathNames(&paths);if(FAILED(hr))throw std::runtime_error("Cannot identify recorder volume paths");
  if(paths){LONG first=0,last=-1;SafeArrayGetLBound(paths,1,&first);SafeArrayGetUBound(paths,1,&last);for(LONG i=first;i<=last;++i){CComVariant path;if(SUCCEEDED(SafeArrayGetElement(paths,&i,&path))&&path.vt==VT_BSTR&&path.bstrVal&&wcslen(path.bstrVal)>=2&&path.bstrVal[1]==L':')keys.push_back(VolumeCoord::DeviceKey(towupper(path.bstrVal[0])-L'A'));}SafeArrayDestroy(paths);}return keys;
 }

 HRESULT Open(const std::wstring& id) override {
  HRESULT hr=recorder_.CoCreateInstance(__uuidof(MsftDiscRecorder2));if(FAILED(hr))return hr;
  hr=recorder_->InitializeDiscRecorder(CComBSTR(id.c_str()));if(FAILED(hr))return hr;
  hr=recorder_->AcquireExclusiveAccess(VARIANT_FALSE,CComBSTR(L"HFVExplorer"));if(FAILED(hr))return hr;exclusive_=true;
  hr=writer_.CoCreateInstance(__uuidof(MsftDiscFormat2Data));if(FAILED(hr))return hr;
  hr=writer_->put_Recorder(recorder_);if(FAILED(hr))return hr;
  return writer_->put_ClientName(CComBSTR(L"HFVExplorer"));
 }
 HRESULT Inspect(CdBurnMedia& media) override {
  VARIANT_BOOL supported=VARIANT_FALSE,blank=VARIANT_FALSE;LONG free=0,next=-1;
  IMAPI_MEDIA_PHYSICAL_TYPE type;IMAPI_FORMAT2_DATA_MEDIA_STATE state;IMAPI_MEDIA_WRITE_PROTECT_STATE protection;
  HRESULT hr=writer_->IsRecorderSupported(recorder_,&supported);if(FAILED(hr)||!supported)return FAILED(hr)?hr:E_NOTIMPL;
  hr=writer_->IsCurrentMediaSupported(recorder_,&supported);if(FAILED(hr))return hr;if(!supported)return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  hr=writer_->get_CurrentPhysicalMediaType(&type);if(FAILED(hr))return hr;
  hr=writer_->get_CurrentMediaStatus(&state);if(FAILED(hr))return hr;
  hr=writer_->get_WriteProtectStatus(&protection);if(FAILED(hr))return hr;
  hr=writer_->get_MediaHeuristicallyBlank(&blank);if(FAILED(hr))return hr;
  hr=writer_->get_NextWritableAddress(&next);if(FAILED(hr))return hr;
  hr=writer_->get_FreeSectorsOnMedia(&free);if(FAILED(hr))return hr;
  media.supported=(type==IMAPI_MEDIA_TYPE_CDR||type==IMAPI_MEDIA_TYPE_CDRW)&&protection==0;
  media.blank=blank&&(state&IMAPI_FORMAT2_DATA_MEDIA_STATE_BLANK)&&!(state&IMAPI_FORMAT2_DATA_MEDIA_STATE_NON_EMPTY_SESSION)&&next==0;
  media.freeSectors=free>0?static_cast<unsigned long long>(free):0;return S_OK;
 }
 HRESULT ConfigureVerifiedFinalizedWrite() override {
  HRESULT hr=writer_->put_ForceOverwrite(VARIANT_FALSE);if(FAILED(hr))return hr;
  hr=writer_->put_ForceMediaToBeClosed(VARIANT_TRUE);if(FAILED(hr))return hr;
  hr=writer_->put_BufferUnderrunFreeDisabled(VARIANT_FALSE);if(FAILED(hr))return hr;
  CComPtr<IBurnVerification> verification;hr=writer_->QueryInterface(IID_PPV_ARGS(&verification));if(FAILED(hr))return hr;
  return verification->put_BurnVerificationLevel(IMAPI_BURN_VERIFICATION_FULL);
 }
 HRESULT Write(IStream* image,Storage::Job& job) override {
  CComPtr<IConnectionPointContainer> container;HRESULT hr=writer_->QueryInterface(IID_PPV_ARGS(&container));if(FAILED(hr))return hr;
  CComPtr<IConnectionPoint> point;hr=container->FindConnectionPoint(__uuidof(DDiscFormat2DataEvents),&point);if(FAILED(hr))return hr;
  CComPtr<IDispatch> sink;sink.Attach(new BurnEvents(job,writer_));DWORD cookie=0;
  hr=point->Advise(sink,&cookie);if(FAILED(hr))return hr;
  hr=job.cancelled()?E_IMAPI_REQUEST_CANCELLED:writer_->Write(image);
  HRESULT detach=point->Unadvise(cookie);return FAILED(hr)?hr:detach;
 }
};
struct Image {
 HANDLE handle=INVALID_HANDLE_VALUE;CComPtr<IStream> stream;BY_HANDLE_FILE_INFORMATION identity={};unsigned long long bytes=0;
 ~Image(){stream.Release();if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
 HRESULT Open(const std::string& path){
  handle=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(handle==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
  if(GetFileType(handle)!=FILE_TYPE_DISK||!GetFileInformationByHandle(handle,&identity))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  bytes=(static_cast<unsigned long long>(identity.nFileSizeHigh)<<32)|identity.nFileSizeLow;
  if(!bytes||bytes%2048||bytes/2048>LONG_MAX)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  return SHCreateStreamOnFileEx(CA2W(path.c_str()),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream);
 }
};
bool same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&CompareFileTime(&a.ftLastWriteTime,&b.ftLastWriteTime)==0;}
std::shared_ptr<CdBurnBackend> backend(std::shared_ptr<CdBurnBackend> injected){
#ifdef HFV_TESTING
 return injected; // Never fall through from a synthetic test to a recorder.
#else
 return injected?injected:std::make_shared<NativeBurn>();
#endif
}
Storage::Result validate(CdBurnBackend& burner,const std::wstring& recorder,unsigned long long bytes){
 HRESULT hr=burner.Open(recorder);if(FAILED(hr))return error(hr,"Opening recorder");
 CdBurnMedia media;hr=burner.Inspect(media);if(FAILED(hr))return error(hr,"Checking recorder/media");
 if(!media.supported)return {1,ERROR_NOT_SUPPORTED,"Insert a supported, writable CD-R or CD-RW."};
 if(!media.blank)return {1,ERROR_WRITE_PROTECT,"A blank disc is required. Existing discs are not erased, overwritten or appended."};
 if(bytes/2048>media.freeSectors)return {1,ERROR_DISK_FULL,"The image is larger than the blank disc capacity."};
 return {};
}
}
std::function<Storage::Result(Storage::Job&)> ListCdRecorders(std::shared_ptr<std::vector<CdRecorder>> output){
 return [=](Storage::Job&)->Storage::Result {
  Apartment com;if(FAILED(com.result))return error(com.result,"Initializing Windows recording");
  CComPtr<IDiscMaster2> master;HRESULT hr=master.CoCreateInstance(__uuidof(MsftDiscMaster2));if(FAILED(hr))return error(hr,"Enumerating recorders");
  CComPtr<IDiscFormat2Data> capability;hr=capability.CoCreateInstance(__uuidof(MsftDiscFormat2Data));if(FAILED(hr))return error(hr,"Checking recorder support");
  LONG count=0;hr=master->get_Count(&count);if(FAILED(hr))return error(hr,"Enumerating recorders");
  std::vector<CdRecorder> recorders;
  for(LONG n=0;n<count;++n){
   CComBSTR id;hr=master->get_Item(n,&id);if(FAILED(hr))return error(hr,"Reading recorder identity");
   if(!id||!SysStringLen(id))continue;
   CComPtr<IDiscRecorder2> recorder;hr=recorder.CoCreateInstance(__uuidof(MsftDiscRecorder2));if(FAILED(hr))continue;
   if(FAILED(recorder->InitializeDiscRecorder(id)))continue;
   VARIANT_BOOL supported=VARIANT_FALSE;if(FAILED(capability->IsRecorderSupported(recorder,&supported))||!supported)continue;
   CComBSTR vendor,product;recorder->get_VendorId(&vendor);recorder->get_ProductId(&product);
   std::string name=vendor?CW2A(vendor):"";name+=" ";name+=product?CW2A(product):"";
   SAFEARRAY* paths=nullptr;
   if(SUCCEEDED(recorder->get_VolumePathNames(&paths))&&paths){
    LONG first=0,last=-1;SafeArrayGetLBound(paths,1,&first);SafeArrayGetUBound(paths,1,&last);
    for(LONG i=first;i<=last;++i){CComVariant path;if(SUCCEEDED(SafeArrayGetElement(paths,&i,&path))&&path.vt==VT_BSTR){name+=" ";name+=CW2A(path.bstrVal);}}
    SafeArrayDestroy(paths);
   }
   recorders.push_back({std::wstring(id,SysStringLen(id)),name});
  }
  *output=std::move(recorders);return {};
 };
}
std::function<Storage::Result(Storage::Job&)> PrepareCdBurn(std::string image,std::wstring recorder,std::shared_ptr<CdBurnPlan> output,std::shared_ptr<CdBurnBackend> injected){
 return [=](Storage::Job& job)->Storage::Result {
  Apartment com;if(FAILED(com.result))return error(com.result,"Initializing Windows recording");
  if(job.cancelled())throw Storage::Cancelled();
  auto burner=backend(injected);if(!burner)return error(E_NOTIMPL,"Missing simulated recorder");auto resources=burner->Resources(recorder);std::vector<VolumeCoord::Access> accesses{{VolumeCoord::ImageKey(image),VolumeCoord::Mode::Read}};for(const auto& key:resources)accesses.push_back({key,VolumeCoord::Mode::Write});std::unique_ptr<VolumeCoord::OperationLease> reservation;try{reservation.reset(new VolumeCoord::OperationLease(accesses));}catch(const VolumeCoord::Busy&){return CommandStorage::Busy();}
  struct CloseBurner{CdBurnBackend* backend;~CloseBurner(){backend->Close();}}close{burner.get()};
  Image file;HRESULT hr=file.Open(image);if(FAILED(hr))return error(hr,"Opening image (use a nonempty raw image, a multiple of 2048 bytes, closed to other writers)");
  auto result=validate(*burner,recorder,file.bytes);if(result.code)return result;
  *output={image,recorder,file.bytes,file.identity,resources};return {};
 };
}
std::function<Storage::Result(Storage::Job&)> MakeCdBurnJob(CdBurnPlan plan,std::shared_ptr<CdBurnBackend> injected){
 return [=](Storage::Job& job)->Storage::Result {
  Apartment com;if(FAILED(com.result))return error(com.result,"Initializing Windows recording");
  if(job.cancelled())throw Storage::Cancelled();publish(job,"Checking source image");
  auto progress=job.progress();progress.path=plan.image;job.progress(progress);
  auto burner=backend(injected);if(!burner)return error(E_NOTIMPL,"Missing simulated recorder");auto resources=burner->Resources(plan.recorder);if(!plan.resources.empty()&&resources!=plan.resources)return {1,ERROR_MEDIA_CHANGED,"Recorder volume paths changed after preparation. Select the recorder again."};std::vector<VolumeCoord::Access> accesses{{VolumeCoord::ImageKey(plan.image),VolumeCoord::Mode::Read}};for(const auto& key:resources)accesses.push_back({key,VolumeCoord::Mode::Write});std::unique_ptr<VolumeCoord::OperationLease> reservation;try{reservation.reset(new VolumeCoord::OperationLease(accesses));}catch(const VolumeCoord::Busy&){return CommandStorage::Busy();}
  struct CloseBurner{CdBurnBackend* backend;~CloseBurner(){backend->Close();}}close{burner.get()};
  Image file;HRESULT hr=file.Open(plan.image);if(FAILED(hr))return error(hr,"Locking source image");
  if(file.bytes!=plan.bytes||!same(file.identity,plan.identity))return {1,ERROR_FILE_INVALID,"The source image changed after preparation. Select it again before burning."};
  publish(job,"Checking blank disc");auto result=validate(*burner,plan.recorder,file.bytes);if(result.code)return result;
  hr=burner->ConfigureVerifiedFinalizedWrite();if(FAILED(hr))return error(hr,"Enabling finalization and full burn verification");
  if(job.cancelled())throw Storage::Cancelled();publish(job,"Preparing to burn");
  hr=burner->Write(file.stream,job);if(FAILED(hr))return error(hr,"Burning/verifying disc",true);
  // A late cancel request must not hide IMAPI's successful verified completion.
  publish(job,"Burn and full verification completed",file.bytes,file.bytes);return {};
 };
}
