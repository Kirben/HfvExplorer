// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "LauncherWriteBack.h"
#include "BrowserPresentation.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "VolumeCoordinator.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
#include "DiskImageOpen.h"
#include "FakeFloppy.h"
#include "floppy.h"
#include <cstdio>
#include <atomic>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
int do_delete_directory(hfsvol*,char*);
}
namespace {
using namespace FileOperations;
using namespace MacBrowser;
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
struct Gate {
 HANDLE event=CreateEvent(nullptr,TRUE,FALSE,nullptr);std::shared_ptr<Storage::Job> job;
 Gate(){Check(event!=nullptr,"create queue gate");job=Storage::worker().submit([this](Storage::Job&){Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"queue gate timed out");return Storage::Result{};});}
 void Release(){SetEvent(event);job->wait();}
 ~Gate(){Release();CloseHandle(event);}
};
std::wstring W(const std::string& s){return {s.begin(),s.end()};}
std::shared_ptr<Volume> Image(const std::wstring& path,const std::u16string& label,HfsPlus::Volume::FormatKind kind){auto file=HfsPlus::CreateImage(path.c_str(),8*1024*1024);HfsPlus::Volume::Format(*file,label,true,kind);file.reset();return OpenImage(path);}
struct Delayed:Volume {
 std::shared_ptr<Volume> inner;DWORD ui=GetCurrentThreadId();unsigned delay=0;bool failRead=false,wrongThread=false;uint32_t failRemove=0;bool cancelRemove=false,loseMedia=false;CWnd* owner=nullptr;CWnd* other=nullptr;bool onlyOwnerDisabled=false;
 explicit Delayed(std::shared_ptr<Volume> v):inner(std::move(v)){}
 void Thread(){wrongThread|=GetCurrentThreadId()==ui;}
 std::string ResourceKey()const override{return inner->ResourceKey();}
 std::u16string Name()const override{return inner->Name();}const char* Kind()const override{return inner->Kind();}bool Writable()const override{return inner->Writable();}bool CaseSensitive()const override{return inner->CaseSensitive();}uint64_t FreeBytes()const override{return inner->FreeBytes();}
 Entry Stat(uint32_t id)override{Thread();return inner->Stat(id);}std::vector<Entry> List(uint32_t p)override{Thread();return inner->List(p);}
 void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{Thread();if(delay){onlyOwnerDisabled=owner&&!owner->IsWindowEnabled()&&other&&other->IsWindowEnabled();Sleep(delay);}if(failRead&&n)throw HfsPlus::Error("Injected slow read error");inner->Read(id,f,at,p,n);}
 uint32_t MakeDirectory(uint32_t p,const std::u16string& n)override{Thread();return inner->MakeDirectory(p,n);}
 uint32_t Put(uint32_t p,const Entry& e,Reader r,bool replace)override{Thread();return inner->Put(p,e,r,replace);}
 void Rename(uint32_t id,uint32_t p,const std::u16string& n)override{Thread();inner->Rename(id,p,n);}
 void ValidateRemoval(uint32_t id)override{Thread();inner->ValidateRemoval(id);}
 void Remove(uint32_t id)override{
  Thread();
  if(failRemove==id){
   if(cancelRemove)throw Storage::Cancelled();
   if(loseMedia){
    Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Failed);
    throw Storage::MediaChanged("Injected media replacement during delete");
   }
   throw HfsPlus::Error("Injected delete error");
  }
  inner->Remove(id);
 }
};
uint32_t Put(Volume& volume,const std::u16string& name){Entry e;e.name=name;e.forks[0].size=4096;e.forks[1].size=17;return volume.Put(2,e,[](unsigned f,uint64_t at,void* p,size_t n){auto bytes=(BYTE*)p;for(size_t i=0;i<n;++i)bytes[i]=BYTE(at+i+f*31);},false);}
Location Native(const std::shared_ptr<Volume>& volume,uint32_t id,const Entry* expected=nullptr){Location out;out.SetMounted(volume,id);if(expected){out.expectedIdentity=expected->identity;out.expectedName=expected->name;out.expectedParent=expected->parent;}return out;}
Location Host(const std::string& path,bool directory=false){Location out;out.SetWindows(W(path));out.directory=directory;return out;}
void Wait(const Operation& op){Check(WaitForSingleObject(op.Job()->event(),15000)==WAIT_OBJECT_0,"operation timed out");}
void Pass(const Operation& op){Wait(op);if(op.Job()->result().code)throw HfsPlus::Error(op.Job()->result().message);Check(op.Snapshot().complete,"operation incomplete");}
void Conflict(const std::shared_ptr<Storage::Job>& job){DWORD until=GetTickCount()+5000;while(!job->done()&&job->conflict().empty()&&LONG(until-GetTickCount())>0)Sleep(10);Check(!job->conflict().empty(),"expected overwrite conflict");}
std::vector<BYTE> Load(const std::string& path){CFile file;Check(file.Open(path.c_str(),CFile::modeRead|CFile::shareDenyNone)!=FALSE,"read fixture");std::vector<BYTE> bytes(size_t(file.GetLength()));Check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"read fixture bytes");return bytes;}
void Save(const std::string& path,const char* data){CFile file;Check(file.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"write fixture");file.Write(data,UINT(strlen(data)));}
void VerifyImageReopenIdentity(const std::string& root,FILE* report)
{
 for(const bool initial:{false,true}){
  const auto path=root+(initial?"\\initial-open.dsk":"\\writable-reopen.dsk");
  const auto parked=path+".original",decoy=path+".replacement";
  const auto format=[](const std::string& name,const std::u16string& label){
   auto file=HfsPlus::CreateImage(W(name).c_str(),8*1024*1024);
   HfsPlus::Volume::Format(*file,label,true,HfsPlus::Volume::FormatKind::HfsPlus);
  };
  format(path,u"Selected");
  format(decoy,u"Replacement");
  const auto originalBytes=Load(path),replacementBytes=Load(decoy);
  bool swapped=false;
  const auto substitute=[&](const std::wstring& opening){
   if(swapped||opening!=W(path))return;
   Check(MoveFileA(path.c_str(),parked.c_str())&&MoveFileA(decoy.c_str(),path.c_str()),
         "substitute same-geometry disk image fixture");
   swapped=true;
  };
  struct Reset{
   ~Reset(){DiskImage::beforeReopenTest={};VolumeCoord::SetPathLookupObserver({});}
  } reset;
  if(initial)VolumeCoord::SetPathLookupObserver(substitute);
  else DiskImage::beforeReopenTest=substitute;
  bool refused=false;
  try{auto volume=OpenImage(W(path));}
  catch(const Storage::MediaChanged&){refused=true;}
  Check(swapped&&refused&&Load(path)==replacementBytes&&Load(parked)==originalBytes,
        "opening a substituted image did not fail or changed one of the files");
  const auto replacement=OpenImage(W(path));
  Check(replacement&&replacement->Name()==u"Replacement","explicit retry could not open the replacement");
 }
 auto path=root+"\\classic-mount.dsk",replacementPath=path+".replacement";
 const auto formatClassic=[](std::string& name,char* label){
  {CFile file;Check(file.Open(name.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic identity fixture");
   file.SetLength(4*1024*1024);}
  Check(hfs_format(name.data(),0,label)==0,"format classic identity fixture");
 };
 char oldLabel[]="Chosen Classic",newLabel[]="Replacement Classic";
 formatClassic(path,oldLabel);
 formatClassic(replacementPath,newLabel);
 const auto expected=VolumeCoord::ImageKey(W(path));
 const auto parked=path+".original";
 const auto original=Load(path),replacement=Load(replacementPath);
 Check(MoveFileA(path.c_str(),parked.c_str())&&MoveFileA(replacementPath.c_str(),path.c_str()),
       "substitute classic HFS mount fixture");
 bool refused=false;
 try{auto volume=OpenClassicImage(W(path),true,expected);}
 catch(const Storage::MediaChanged&){refused=true;}
 Check(refused&&Load(path)==replacement&&Load(parked)==original,"classic HFS accepted a different file descriptor");
 const auto legacy=root+"\\legacy-classic-mount.dsk",legacyParked=legacy+".original";
 auto legacyPath=legacy,legacyDecoy=legacy+".replacement";
 formatClassic(legacyPath,oldLabel);
 formatClassic(legacyDecoy,newLabel);
 const auto legacyOriginal=Load(legacy),legacyReplacement=Load(legacyDecoy);
 auto folder=Location::Classic(legacy,":");
 folder.expectedResource=folder.ResourceKey();
 bool swapped=false;
 struct ClearObserver { ~ClearObserver(){VolumeCoord::SetPathLookupObserver({});} } clear;
 VolumeCoord::SetPathLookupObserver([&](const std::wstring& opening){
  if(swapped||opening!=W(legacy))return;
  Check(MoveFileA(legacy.c_str(),legacyParked.c_str())&&MoveFileA(legacyDecoy.c_str(),legacy.c_str()),
        "substitute legacy classic HFS image after identity lookup");
  swapped=true;
 });
 // The first pathname lookup selected the old image. A later open must
 // verify its descriptor before libhfs is allowed to read or recover it.
 refused=false;
 try{OpenClassic(legacy,":",true);}
 catch(const Storage::MediaChanged&){refused=true;}
 Check(swapped&&refused,"Legacy classic HFS accepted a substituted path");
 Storage::Job job;
 Completion completion;
 auto rejected=ExecuteInWorker(CreateFolder{folder,u"Wrong volume"},job,completion);
 Check(rejected.error==ERROR_MEDIA_CHANGED&&Load(legacy)==legacyReplacement&&Load(legacyParked)==legacyOriginal,
       "A stale legacy HFS mutation touched the original or replacement image");
 auto explicitRetry=OpenClassic(legacy,":",true);
 Check(explicitRetry->Name()==u"Replacement Classic","An explicit legacy mount could not select the new image");
 explicitRetry->Close();
 fprintf(report,"PASS: HFS+ open/reopen and classic HFS session and legacy mounts reject substituted images before recovery or mutation. Both files remain unchanged.\n");
}
void WindowsStagingSafety(const std::string& root,FILE* report)
{
 for(const auto phase:{"export-files-reserved","staged","metadata-file-reserved"}){
  const auto folder=root+"\\swap-"+phase;
  Check(CreateDirectoryA(folder.c_str(),nullptr)!=FALSE,"create staging substitution fixture");
  auto host=OpenWindows(W(folder));
  const auto id=Put(*host,u"Original");
  host->Close();
  const auto data=folder+"\\Original",companion=folder+"\\%Original";
  const auto priorData=Load(data),priorCompanion=Load(companion);
  std::string decoy;
  bool replaced=false;
  bool rejected=false;
  struct Reset{~Reset(){windowsPublicationFault={};}} reset;
  windowsPublicationFault=[&](const char* current){
   if(strcmp(current,phase))return;
   auto stagingDirectory=folder;
   auto pattern="\\HFV*.tmp";
   if(!strcmp(phase,"metadata-file-reserved")){
    WIN32_FIND_DATAA journal{};
    const auto search=FindFirstFileA((folder+"\\.hfv-export-*").c_str(),&journal);
    Check(search!=INVALID_HANDLE_VALUE,"locate metadata recovery directory");
    FindClose(search);
    stagingDirectory+="\\"+std::string(journal.cFileName);
    pattern="\\1"; // The companion is the first owned slot.
   }
   WIN32_FIND_DATAA found{};
   const auto search=FindFirstFileA((stagingDirectory+pattern).c_str(),&found);
   Check(search!=INVALID_HANDLE_VALUE,"locate reserved staging file");
   FindClose(search);
   decoy=stagingDirectory+"\\"+found.cFileName;
   Check(MoveFileA(decoy.c_str(),(decoy+".original").c_str())!=FALSE,"move reserved staging file aside");
   Save(decoy,"UNRELATED");
   replaced=true;
  };
  try{
   if(!strcmp(phase,"metadata-file-reserved"))host->SetAttributes(id,{{u"user.New",{1,2,3}}});
   else{
    auto entry=host->Stat(id);
    host->Put(2,entry,[](unsigned,uint64_t,void* out,size_t count){memset(out,0x81,count);},true);
   }
  }catch(const HfsPlus::Error&){rejected=true;}
  windowsPublicationFault={};
  Check(replaced&&rejected&&Load(decoy)==std::vector<BYTE>({'U','N','R','E','L','A','T','E','D'})&&
        Load(data)==priorData&&Load(companion)==priorCompanion,
        "Windows staging truncated, published or removed an unrelated replacement");
 }
 fprintf(report,"PASS: export and metadata staging refuse substituted files before writing and publication, preserving both original forks.\n");
}
void WindowsCutSafety(const std::string& root,const std::shared_ptr<Volume>& native,FILE* report)
{
 auto image=root+"\\cut-race.dsk";
 {
  CFile file;
  Check(file.Open(image.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic cut fixture");
  file.SetLength(4*1024*1024);
 }
 char label[]="Cut tests";
 Check(hfs_format(image.data(),0,label)==0,"format classic cut fixture");
 const auto classic=OpenImage(W(image));
 for(const auto& destination:{native,classic}){
  const bool coordinated=destination==classic;
  for(const bool replace:{false,true}){
   const auto parent=root+(coordinated?"\\classic-cut-":"\\native-cut-")+(replace?"replace":"new");
   const auto subtree=parent+"\\Transfer",nested=subtree+"\\Nested",file=nested+"\\Original";
   const auto unrelated=replace?file:nested+"\\Unrelated";
   Check(CreateDirectoryA(parent.c_str(),nullptr)&&CreateDirectoryA(subtree.c_str(),nullptr)&&
         CreateDirectoryA(nested.c_str(),nullptr),"create Windows cut safety fixture");
   Save(file,"ORIGINAL");
   auto source=Host(subtree,true);
   source.cut=true;
   const auto folder=destination->MakeDirectory(2,coordinated?(replace?u"Replace C":u"New C"):(replace?u"Replace N":u"New N"));
   bool injected=false;
   struct Reset{~Reset(){windowsPublicationFault={};}} reset;
   windowsPublicationFault=[&](const char* phase){
    if(strcmp(phase,"cut-source-verified"))return;
    if(replace)Check(MoveFileA(file.c_str(),(parent+"\\Saved original").c_str()),"replace a verified source entry");
    Save(unrelated,"UNRELATED");
    injected=true;
   };
   Storage::Job job;
   Completion completion;
   const auto result=ExecuteInWorker(OrdinaryCopy{{source},Native(destination,folder)},job,completion);
   windowsPublicationFault={};
   Check(injected&&result.code&&!result.moved&&
         Load(unrelated)==std::vector<BYTE>({'U','N','R','E','L','A','T','E','D'}),
         "Windows cut discarded a file that was absent from the verified source version");
   if(coordinated){
    Check(destination->List(folder).empty(),"coordinated cut failure did not roll back its destination");
    if(!replace)Check(Load(file)==std::vector<BYTE>({'O','R','I','G','I','N','A','L'}),
                      "coordinated cut failure removed an original source entry");
   }else{
    const auto destinationTree=destination->List(folder);
    Check(destinationTree.size()==1&&destinationTree.front().name==u"Transfer",
          "a non-coordinated Cut lost the copied destination after source cleanup was refused");
   }
  }
 }
 for(bool replaceDestination:{false,true}){
  const auto folder=root+(replaceDestination?"\\destination-replaced":"\\destination-removed");
  Check(CreateDirectoryA(folder.c_str(),nullptr)!=FALSE,"create Windows move destination fixture");
  const auto sourcePath=root+"\\Cut original",copied=folder+"\\Cut original";
  Save(sourcePath,"ORIGINAL");
  auto source=Host(sourcePath,false);
  source.cut=true;
  bool injected=false;
  Storage::Job job;
  job.transport([&](const Storage::Progress& progress){
   if(injected||progress.phase!="Verifying Windows destination before Cut cleanup")return;
   injected=true;
   Check(DeleteFileA(copied.c_str())!=FALSE,"remove published Windows copy");
   if(replaceDestination)Save(copied,"REPLACEMENT");
  },{});
  Completion completion;
  const auto result=ExecuteInWorker(OrdinaryCopy{{source},Host(folder,true)},job,completion);
  Check(injected&&result.code&&!result.moved&&
        Load(sourcePath)==std::vector<BYTE>({'O','R','I','G','I','N','A','L'}),
        "changed Windows destination did not stop Cut cleanup");
  if(replaceDestination)
   Check(Load(copied)==std::vector<BYTE>({'R','E','P','L','A','C','E','M','E','N','T'}),
         "Cut cleanup modified the replacement destination");
 }
 fprintf(report,"PASS: coordinated and ordinary Windows cuts retain new and replaced files at source cleanup.\n");
 fprintf(report,"PASS: ordinary Cut retains the original when a published Windows destination is removed or replaced.\n");
}
void WindowsStreamSafety(const std::string& root,const std::shared_ptr<Volume>& mac,FILE* report)
{
 const auto winFolder=root+"\\streams";
 const auto windowsCopy=root+"\\streams-copy";
 Check(CreateDirectoryA(winFolder.c_str(),nullptr)&&CreateDirectoryA(windowsCopy.c_str(),nullptr),
       "create NTFS stream test folders");
 const auto file=winFolder+"\\Payload ADS";
 Save(file,"DATA");
 Save(file+":extra","PRIVATE");
 const auto execute=[&](const std::string& path,const Location& destination,bool cut){
  Storage::Job job;
  job.transport({},[](const auto&){return Storage::ConflictChoice::Replace;});
  auto source=Host(path);
  source.cut=cut;
  Completion completion;
  return ExecuteInWorker(OrdinaryCopy{{source},destination},job,completion);
 };
 const auto destination=Native(mac,mac->MakeDirectory(2,u"Windows streams"));
 const auto refused=execute(file,destination,true);
 Check(refused.code&&refused.message.find("alternate data streams")!=std::string::npos&&
       Load(file+":extra").size()==7&&mac->List(destination.Id()).empty(),
       "Mac cut discarded Windows streams or wrote a partial destination");
 Check(!execute(file,destination,false).code&&Load(file+":extra").size()==7,
       "copy-only import should leave the original stream intact");
 Check(!execute(file,Host(windowsCopy),true).code&&GetFileAttributesA(file.c_str())==INVALID_FILE_ATTRIBUTES&&
       Load(windowsCopy+"\\Payload ADS:extra")==std::vector<BYTE>({'P','R','I','V','A','T','E'}),
       "Windows file move failed to preserve the named stream");
 const auto directory=winFolder+"\\Directory";
 Check(CreateDirectoryA(directory.c_str(),nullptr)!=FALSE,"create folder stream fixture");
 Save(directory+"\\Child","KEEP");
 Save(directory+":metadata","FOLDER");
 Check(execute(directory,Host(windowsCopy),true).code&&Load(directory+":metadata").size()==6&&
       GetFileAttributesA((windowsCopy+"\\Directory").c_str())==INVALID_FILE_ATTRIBUTES,
       "a Windows folder move discarded a directory stream");

 auto host=OpenWindows(W(winFolder));
 Entry entry;
 entry.name=u"Fork and streams";
 entry.forks[0].size=4;
 entry.forks[1].size=3;
 memcpy(entry.finder.data(),"TEXTTEST",8);
 const auto id=host->Put(2,entry,[](unsigned fork,uint64_t,void* data,size_t n){
  memcpy(data,fork?"OLD":"DATA",n);
 },false);
 const auto item=winFolder+"\\Fork and streams";
 const auto companion=winFolder+"\\%Fork and streams";
 Save(item+":primary","PRIMARY");
 Save(companion+":companion","COMPANION");
 auto original=host->Stat(id);
 HfsPlus::Attributes attrs;
 attrs[u"user.audit"]={7,5,3};
 host->SetAttributes(id,attrs);
 Check(Load(companion+":companion").size()==9,"attribute edit dropped companion stream");
 const auto input=winFolder+"\\Resource input";
 Save(input,"NEWFORK");
 Pass(Submit(ImportResourceFork{Host(input),Host(item)}));
 auto replaced=host->Stat(id);
 char resource[7]{};
 host->Read(id,1,0,resource,sizeof(resource));
 Check(replaced.identity.primary==original.identity.primary&&Load(item)==std::vector<BYTE>({'D','A','T','A'})&&
       Load(item+":primary").size()==7&&Load(companion+":companion").size()==9&&
       !memcmp(resource,"NEWFORK",7)&&host->GetAttributes(id)==attrs,
       "Windows resource-fork replacement changed unrelated data or streams");
 fprintf(report,"PASS: Windows cuts retain unsupported streams and preserve transferable ones; metadata and resource edits keep the primary file and both NTFS streams.\n");
}
#ifdef HFV_FLOPPY_TESTING
class OperationFloppy:public FloppyWriteApi {
public:
 FakeFloppy disk;bool locked=false,denyLock=false,denyWrites=false;unsigned writes=0;
 explicit OperationFloppy(const std::vector<BYTE>& data):disk(data){}
 UINT DriveType(int d)override{return disk.DriveType(d);}HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{return disk.Open(p,a,s,f);}
 bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk.Geometry(h,g);}bool Verify(HANDLE h,DWORD& n)override{return disk.Verify(h,n);}bool Read(HANDLE h,ULONGLONG at,DWORD n,void* p,DWORD& got)override{return disk.Read(h,at,n,p,got);}void Close(HANDLE h)override{disk.Close(h);}
 bool Lock(HANDLE)override{if(denyLock){SetLastError(ERROR_ACCESS_DENIED);return false;}locked=true;return true;}bool Dismount(HANDLE)override{return locked;}bool Unlock(HANDLE)override{locked=false;return true;}
 bool Write(HANDLE,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{Check(locked&&at<=disk.media.size()&&n<=disk.media.size()-at,"unsafe simulated floppy write");if(denyWrites){got=0;SetLastError(ERROR_WRITE_FAULT);return false;}memcpy(disk.media.data()+size_t(at),p,n);got=n;++writes;return true;}
 bool Flush(HANDLE)override{SetLastError(ERROR_INVALID_FUNCTION);return false;}
};
#endif
struct Tree:HFVExplorerTreeView {void Init(){Initialize();enable_selchange(0);}void PostNcDestroy()override{};};
struct List:CHFVExplorerListView {void PostNcDestroy()override{};};
}
int RunFileOperationsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;FILE* report=fopen((root+"\\file-operations-report.txt").c_str(),"w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());CWnd owner,other;InitializeStorageUi();int result=0;
 try{
  Check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden operation owner",WS_OVERLAPPED,0,0,600,400,nullptr,0)&&other.CreateEx(0,AfxRegisterWndClass(0),"Other browser",WS_OVERLAPPED,0,0,600,400,nullptr,0),"create hidden owners");app.m_pMainWnd=&other;app.m_mac_window_mode=0;
  auto source=Image(W(root+"\\source.dsk"),u"Source",HfsPlus::Volume::FormatKind::HfsPlus),target=Image(W(root+"\\target.dsk"),u"Target",HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
  VerifyImageReopenIdentity(root,report);
  WindowsCutSafety(root,target,report);
  WindowsStreamSafety(root,target,report);
  uint32_t file=Put(*source,u"Payload"),folder=target->MakeDirectory(2,u"Copies");auto expected=source->Stat(file);auto delayed=std::make_shared<Delayed>(source);delayed->delay=120;delayed->owner=&owner;delayed->other=&other;
  Request copy;copy.Copy().sources={Native(delayed,file,&expected)};copy.Copy().destination=Native(target,folder);
  // Destroy the originating session and its panes while its captured request waits.
  std::weak_ptr<Volume> retained=delayed;Operation detached=[&]{Gate gate;Tree tree;List list;Operation operation=[&]{BrowserSession session;session.Attach(&owner,&tree,&list);Check(tree.Create(nullptr,nullptr,WS_CHILD,CRect(0,0,200,300),&owner,100,nullptr)&&list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,600,300),&owner,101,nullptr),"create originating panes");tree.Init();list.Initialize();session.MacBrowser().AttachTestVolume(delayed,"source");auto op=Submit(copy);copy.Copy().sources[0].SetId(0xffffffff);copy.Copy().destination.SetId(0xffffffff);return op;}();list.DestroyWindow();tree.DestroyWindow();delayed.reset();copy.Copy().sources.clear();gate.Release();return operation;}();
  Pass(detached);auto copied=target->List(folder);Check(copied.size()==1&&copied[0].name==u"Payload"&&copied[0].forks[1].size==17,"captured copy request changed with caller state");Storage::worker().submit([](Storage::Job&){return Storage::Result{};})->wait();Check(retained.expired(),"completed operation retained its source backend through history");
  fprintf(report,"PASS: copy request survives originating session/pane destruction, retains backend lifetime until execution, owns original IDs/options, and releases backend references after completion.\n");
  delayed=std::make_shared<Delayed>(source);delayed->delay=120;delayed->owner=&owner;delayed->other=&other;Request shown;shown.Copy().sources={Native(delayed,file,&expected)};shown.Copy().destination=Native(target,2);auto visible=Submit(shown);storageUiTest={};auto shownResult=ShowSubmittedStorageJob(visible.Job(),"Copying",true,true,true,&owner);Check(!shownResult.code&&visible.Snapshot().complete&&visible.Job()->progress().processed==visible.Job()->progress().totalBytes&&storageUiTest.timerTicks>0&&storageUiTest.copyBarCount==1&&delayed->onlyOwnerDisabled&&owner.IsWindowEnabled()&&other.IsWindowEnabled()&&!delayed->wrongThread,"submitted-job presentation used another owner or blocked UI");
  {Request layout=shown;layout.Copy().destination=Native(target,target->MakeDirectory(2,u"Folder layout"));auto op=Submit(layout);storageUiTest={};auto value=ShowSubmittedStorageJob(op.Job(),"Copying folder",true,false,true,&owner);Check(!value.code&&storageUiTest.copyBarCount==2,"submitted folder copy lost overall/file progress bars");}
  {Request cancel=shown;cancel.Copy().destination=Native(target,target->MakeDirectory(2,u"Cancel layout"));auto op=Submit(cancel);storageUiTest={};storageUiTest.cancelOnTimer=true;auto value=ShowSubmittedStorageJob(op.Job(),"Cancelling copy",true,true,true,&owner);Check(value.error==ERROR_CANCELLED&&owner.IsWindowEnabled()&&other.IsWindowEnabled(),"submitted-job Cancel button or owner restoration");storageUiTest={};}
  Request skip=shown;auto skipped=Submit(skip);Conflict(skipped.Job());skipped.Job()->resolve(Storage::ConflictChoice::Skip);Wait(skipped);Check(!skipped.Job()->result().code&&!skipped.Snapshot().complete&&skipped.Job()->progress().skipped==1,"skipped copy reported fully complete");
  std::shared_ptr<Storage::Job> orphan;{auto abandoned=Submit(shown);orphan=abandoned.Job();}Conflict(orphan);auto pending=Pending();Check(std::any_of(pending.begin(),pending.end(),[&](const Operation& op){return op.Job()==orphan;}),"detached conflict absent from application operation registry");CancelPending();orphan->resolve(Storage::ConflictChoice::Replace);orphan->wait();Check(orphan->result().error==ERROR_CANCELLED&&source->Stat(file).name==u"Payload","detached conflict cancellation lost source");pending.clear();
  {Gate gate;Request remove = FileOperations::DeleteEntries{{Native(delayed,file,&expected)}};auto cancelled=Submit(remove);cancelled.Job()->cancel();gate.Release();Wait(cancelled);Check(cancelled.Job()->result().outcome==Storage::Outcome::Cancelled&&!cancelled.Job()->result().partial&&cancelled.Job()->result().rollback==Storage::Rollback::None&&cancelled.Snapshot().affected.empty()&&source->Stat(file).name==u"Payload","queued cancellation mutated source");}
  RenameEntry rename{Native(delayed,file,&expected), u"Renamed", Native(delayed,2)};
  {Gate gate;auto first=Submit(rename);auto stale=Submit(rename);rename.name=u"Changed after submission";gate.Release();Pass(first);Wait(stale);Check(stale.Job()->result().code&&source->Stat(file).name==u"Renamed","stale queued rename operated on changed selection");}
  auto staleDelete = Submit(DeleteEntries{{rename.source}});
  Wait(staleDelete);
  Check(staleDelete.Job()->result().code && staleDelete.Snapshot().completed == 0 &&
        source->Stat(file).name == u"Renamed", "Stale delete removed a renamed entry");
  const auto occupied = Put(*source, u"Occupied");
  const auto current = source->Stat(file);
  auto collision = Submit(RenameEntry{Native(delayed, file, &current), u"Occupied"});
  Wait(collision);
  Check(collision.Job()->result().code && collision.Snapshot().completed == 0 &&
        source->Stat(file).name == u"Renamed" && source->Stat(occupied).name == u"Occupied",
        "Rename collision changed the source or destination");
  expected=source->Stat(file);delayed->failRead=true;Request bad;bad.Copy().sources={Native(delayed,file,&expected)};bad.Copy().destination=Native(target,folder);bad.Copy().sources[0].cut=true;auto failed=Submit(bad);Wait(failed);Check(failed.Job()->result().code&&!failed.Snapshot().complete&&!failed.Snapshot().affected.empty()&&source->Stat(file).name==u"Renamed","copy failure lost cut source or omitted affected resource");delayed->failRead=false;delayed->delay=0;
  uint32_t one=Put(*source,u"Delete one"),two=Put(*source,u"Delete two");delayed->failRemove=two;Request remove = FileOperations::DeleteEntries{{Native(delayed,one),Native(delayed,two)}};auto partial=Submit(remove);Wait(partial);Check(partial.Job()->result().outcome==Storage::Outcome::Failed&&partial.Job()->result().partial&&partial.Job()->result().completed==1&&!partial.Snapshot().complete&&partial.Snapshot().completed==1&&source->Stat(two).name==u"Delete two"&&!partial.Snapshot().affected.empty(),"partial delete completion was lost");delayed->failRemove=0;
  MacBrowser::Entry deletedEntry;
  Check(!source->FindChild(2, u"Delete one", deletedEntry), "Completed delete was rolled back");
  const auto third=Put(*source,u"Delete before cancellation");
  auto revision=VolumeCoord::Revision(source->ResourceKey());
  delayed->failRemove=two;
  delayed->cancelRemove=true;
  auto cancelled=Submit(DeleteEntries{{Native(delayed,third),Native(delayed,two)}});
  Wait(cancelled);
  const auto stopped=cancelled.Job()->result();
  Check(stopped.outcome==Storage::Outcome::Cancelled&&stopped.partial&&stopped.completed==1&&
        stopped.rollback==Storage::Rollback::None&&VolumeCoord::Revision(source->ResourceKey())==revision+1&&
        !source->FindChild(2,u"Delete before cancellation",deletedEntry)&&source->Stat(two).name==u"Delete two",
        "Partial delete cancellation lost the retained changes or their revision");
  delayed->cancelRemove=false;
  delayed->loseMedia=true;
  revision=VolumeCoord::Revision(source->ResourceKey());
  auto changed=Submit(DeleteEntries{{Native(delayed,two)}});
  Wait(changed);
  const auto mediaError=changed.Job()->result();
  Check(mediaError.outcome==Storage::Outcome::Failed&&mediaError.error==ERROR_MEDIA_CHANGED&&
        mediaError.rollback==Storage::Rollback::Failed&&mediaError.completed==0&&
        VolumeCoord::Revision(source->ResourceKey())==revision+1&&source->Stat(two).name==u"Delete two",
        "Failed rollback or media-change result was lost before refresh");
  delayed->loseMedia=false;
  delayed->failRemove=0;
  ExportFileFork raw{Native(delayed,file,&expected),Host(root+"\\resource.bin"),ForkSelector::Resource};Pass(Submit(raw));Check(Load(root+"\\resource.bin").size()==17,"raw fork export job");auto repeated=Submit(raw);Wait(repeated);Check(repeated.Job()->result().code&&Load(root+"\\resource.bin").size()==17,"fork export overwrote existing output");
  Save(root+"\\replacement.bin","NEW RESOURCE");ImportResourceFork resource{Host(root+"\\replacement.bin"),Native(delayed,file,&expected)};Pass(Submit(resource));Check(source->Stat(file).forks[0].size==4096&&source->Stat(file).forks[1].size==12,"resource import job lost data fork");
  fprintf(report,"PASS: worker-only copy/rename/delete and fork jobs, slow responsive owner-specific progress, overwrite skip, detached conflict discovery/cancellation, queued cancellation, stale rename/delete rejection, collision preservation, cut-source retention, partial-delete accounting and raw-export overwrite preservation.\n");
  // The legacy backend keeps its safe path-based host mutations and converters.
  Save(root+"\\ordinary.txt","KEEP");Save(root+"\\%ordinary.txt","UNRELATED");Request hostRename = FileOperations::RenameEntry{Host(root+"\\ordinary.txt"), u"renamed.txt"};Pass(Submit(hostRename));Check(Load(root+"\\renamed.txt").size()==4,"Windows rename job");Request hostRemove = FileOperations::DeleteEntries{{Host(root+"\\renamed.txt")}};Pass(Submit(hostRemove));Check(Load(root+"\\%ordinary.txt").size()==9,"Windows delete removed unrelated companion");
  std::string classic=root+"\\classic.dsk";{CFile f;Check(f.Open(classic.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic job image");f.SetLength(2*1024*1024);}char label[]="Classic",type[]="TEXT",creator[]="TEST",name[]="Classic:Original";Check(hfs_format(&classic[0],0,label)==0,"format classic job fixture");auto v=hfs_mount(&classic[0],0,O_RDWR);Check(v&&hfs_create(v,name,type,creator)==0,"create classic job source");auto f=hfs_open(v,name);Check(f&&hfs_write(f,const_cast<char*>("DATA"),4)==4&&hfs_close(f)==0&&hfs_umount(v)==0,"write classic source");
  Location classicSource=Location::Classic(classic,"Classic:Original");
  {
   auto original=OpenClassic(classic,"Classic:",true);
   Entry entry;entry.name=u"Launch document";entry.forks[0].size=4;entry.forks[1].size=3;
   memcpy(entry.finder.data(),"TEXTTEST",8);
   original->Put(2,entry,[](unsigned fork,uint64_t,void* data,size_t bytes){
    memcpy(data,fork?"RSR":"DATA",bytes);
   },false);
   original->Close();
   auto location=Location::Classic(classic,"Classic:Launch document");
   location.expectedResource=location.ResourceKey();
   Storage::Job capture;
   auto version=CaptureLaunchVersion(location,capture);
   auto edited=root+"\\edited-launch.txt";
   Save(edited,"EDIT");
   Storage::Job apply;
   Storage::Result updated;
   {
    LauncherTemporaryDocument inspected;
    Check(!inspected.Open(W(edited).c_str(),true).code,"protect temporary launcher edit");
    uint64_t hash=0;
    Check(!inspected.ReadVersion(hash,apply).code,"inspect pinned launcher edit");
    auto writer=CreateFileA(edited.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,0,nullptr);
    if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    Check(writer==INVALID_HANDLE_VALUE&&!MoveFileA(edited.c_str(),(edited+".renamed").c_str()),
          "a pending launcher write-back allowed the approved file to be changed or replaced");
    updated=ApplyLaunchEdit(location,version,inspected.Handle(),apply);
    Check(!updated.code,updated.message.c_str());
    Check(inspected.Remove(),"remove only the imported launcher edit by handle");
   }
   Check(GetFileAttributesA(edited.c_str())==INVALID_FILE_ATTRIBUTES,"imported launcher edit was not removed");
   Check(!updated.code,updated.message.c_str());
   Save(edited,"EDIT");
   Save(edited+":EditorState","NAMED");
   {
    LauncherTemporaryDocument inspected;
    Check(!inspected.Open(W(edited).c_str(),true).code,"protect temporary document with an editor stream");
    Check(inspected.CheckWriteBackStreams().error==ERROR_NOT_SUPPORTED&&!inspected.Remove(),
          "write-back allowed cleanup to discard an editor stream");
   }
   Check(Load(edited)==std::vector<BYTE>({'E','D','I','T'})&&
         Load(edited+":EditorState")==std::vector<BYTE>({'N','A','M','E','D'}),
         "write-back stream protection changed the staged document");
   {
    const auto pendingRoot=root+"\\launcher-recovery",folder=pendingRoot+"\\hfv123.tmp.dir";
    Check(CreateDirectoryA(pendingRoot.c_str(),nullptr)&&CreateDirectoryA(folder.c_str(),nullptr),
          "create launcher recovery fixture");
    const auto pending=folder+"\\Edited copy";
    Save(pending,"WINDOWS EDIT");
    {
     LauncherWriteBack edit(location,version,CString(pending.c_str()));
     Check(edit.SavePending(),"write a durable edit record");
     Check(LauncherWriteBack::LoadPending(CString(pendingRoot.c_str())).empty(),
           "another browser claimed a live pending edit");
    }
    {
     auto recovered=LauncherWriteBack::LoadPending(CString(pendingRoot.c_str()));
     Check(recovered.size()==1&&recovered.front()->TemporaryPath()==pending.c_str()&&
           Load(pending)==std::vector<BYTE>({'W','I','N','D','O','W','S',' ','E','D','I','T'}),
           "pending edit did not survive reopening");
    }
    const auto record=folder+"\\.hfv-pending-edit";
    const auto beforeRetry=Load(record);
    {
     auto recovered=LauncherWriteBack::LoadPending(CString(pendingRoot.c_str()));
     LauncherTemporaryDocument inspected;
     Check(recovered.size()==1&&!inspected.Open(W(pending).c_str(),true).code,"open stale edit for retry");
     for(unsigned i=0;i<2;++i){
      Storage::Job retry;
      Check(recovered.front()->ApplyPending(inspected,retry).code,"stale launcher write succeeded");
     }
    }
    Check(Load(record)==beforeRetry,"rejected write-back attempts allocated another decision");
    fprintf(report,"PASS: stale launcher retries leave the recovery record unchanged.\n");
    const auto stream=pending+":CleanupState";
    Save(stream,"KEEP");
    {
     auto recovered=LauncherWriteBack::LoadPending(CString(pendingRoot.c_str()));
     LauncherTemporaryDocument inspected;
     Check(recovered.size()==1&&!inspected.Open(W(pending).c_str(),true).code&&
           !recovered.front()->FinishUnchanged(inspected),
           "unchanged edit cleaned up despite a pending stream");
    }
    Check(LauncherWriteBack::LoadPending(CString(pendingRoot.c_str())).size()==1&&
          Load(stream)==std::vector<BYTE>({'K','E','E','P'}),
          "failed cleanup no longer appears as a pending edit after reopening");
    Check(DeleteFileA(stream.c_str())!=FALSE,"remove regression fixture stream");
    {
     LauncherTemporaryDocument inspected;
     Check(!inspected.Open(W(pending).c_str(),true).code&&!inspected.Remove([]{return false;}),
           "simulated record retirement failure unexpectedly finished cleanup");
    }
    Check(GetFileAttributesA(pending.c_str())!=INVALID_FILE_ATTRIBUTES&&
          GetFileAttributesA(record.c_str())!=INVALID_FILE_ATTRIBUTES,
          "failed record retirement left the staged copy marked for deletion");
    {
     const auto cleanFolder=pendingRoot+"\\hfv124.tmp.dir";
     const auto cleanCopy=cleanFolder+"\\Unchanged";
     Check(CreateDirectoryA(cleanFolder.c_str(),nullptr)!=FALSE,"create successful cleanup fixture");
     Save(cleanCopy,"UNCHANGED");
     LauncherWriteBack edit(location,version,CString(cleanCopy.c_str()));
     Check(edit.SavePending(),"persist successful cleanup fixture");
     {
      LauncherTemporaryDocument inspected;
      Check(!inspected.Open(W(cleanCopy).c_str(),true).code&&edit.FinishUnchanged(inspected),
            "ordinary staged file and its record did not retire together");
     }
     Check(GetFileAttributesA(cleanCopy.c_str())==INVALID_FILE_ATTRIBUTES&&
           GetFileAttributesA((cleanFolder+"\\.hfv-pending-edit").c_str())==INVALID_FILE_ATTRIBUTES,
           "successful cleanup left the staged file or record behind");
    }
    fprintf(report,"PASS: launcher cleanup failures retain both the staged file and its recoverable record.\n");
    {
     const auto handle=CreateFileA(record.c_str(),FILE_APPEND_DATA,0,nullptr,OPEN_EXISTING,0,nullptr);
     Check(handle!=INVALID_HANDLE_VALUE,"open pending record for missing-marker test");
     const std::string missingMarker(32,'0');
     DWORD written=0;
     const bool wrote=WriteFile(handle,missingMarker.data(),DWORD(missingMarker.size()),&written,nullptr)!=FALSE;
     CloseHandle(handle);
     Check(wrote&&written==missingMarker.size(),"bind pending record to a missing marker");
     auto recovered=LauncherWriteBack::LoadPending(CString(pendingRoot.c_str()));
     Check(recovered.size()==1,"recover edit with a missing marker");
     bool committed=true;
     LauncherTemporaryDocument inspected;
     Storage::Job retry;
     Check(!inspected.Open(W(pending).c_str(),true).code&&
           recovered.front()->CheckCommitStatus(committed).code&&!committed&&
           !recovered.front()->RetireCommitted()&&recovered.front()->ApplyPending(inspected,retry).code,
           "missing decision marker escaped as an exception or permitted a write");
    }
    Check(GetFileAttributesA(record.c_str())!=INVALID_FILE_ATTRIBUTES&&
          GetFileAttributesA(pending.c_str())!=INVALID_FILE_ATTRIBUTES,
          "missing decision marker lost the pending edit or temporary document");
    fprintf(report,"PASS: an unavailable launcher commit marker blocks write-back and preserves the recovery record and copy.\n");
    Check(SetFileAttributesA(record.c_str(),FILE_ATTRIBUTE_NORMAL)!=FALSE,"prepare malformed recovery record");
    Save(record,"TRUNCATED");
    std::vector<CString> preserved;
    Check(LauncherWriteBack::LoadPending(CString(pendingRoot.c_str()),&preserved).empty()&&
          preserved.size()==1&&preserved.front()==folder.c_str()&&
          GetFileAttributesA(record.c_str())!=INVALID_FILE_ATTRIBUTES&&
          GetFileAttributesA(pending.c_str())!=INVALID_FILE_ATTRIBUTES,
          "malformed pending record was hidden or the staged edit was removed or claimed");
    fprintf(report,"PASS: a saved launcher edit survives reopening, cannot be claimed twice, and malformed records keep the staged copy.\n");
   }
   {
    const auto originalPath=root+"\\launcher-hardlink-source",alias=root+"\\launcher-hardlink-alias";
    Save(originalPath,"EXTERNAL");
    Check(SetFileAttributesA(originalPath.c_str(),FILE_ATTRIBUTE_READONLY)&&
          ::CreateHardLinkA(alias.c_str(),originalPath.c_str(),nullptr),"create read-only launcher hard-link fixture");
    LauncherTemporaryDocument document;
    Check(document.Open(W(alias).c_str(),true).code&&!document.Remove()&&
          Load(alias)==Load(originalPath)&&
          (GetFileAttributesA(originalPath.c_str())&FILE_ATTRIBUTE_READONLY),
          "launcher accepted an alias or changed the shared file");
    fprintf(report,"PASS: launcher refuses a hard-linked staged document and preserves the external alias and its attributes.\n");
   }
   Save(edited,"LATE");
   Storage::Job stale;
   LauncherTemporaryDocument lateEdit;
   Check(!lateEdit.Open(W(edited).c_str(),false).code,"protect stale launcher edit");
   Check(ApplyLaunchEdit(location,version,lateEdit.Handle(),stale).code&&
         Load(edited)==std::vector<BYTE>({'L','A','T','E'}),
         "stale launcher write-back was accepted or discarded edits");
   auto reopened=OpenClassic(classic,"Classic:",false);
   Entry actual;
   Check(reopened->FindChild(2,entry.name,actual)&&actual.forks[0].size==4&&actual.forks[1].size==3&&
         !memcmp(actual.finder.data(),"TEXTTEST",8),"launcher write-back did not preserve metadata");
   char data[4]{},resource[3]{};
   reopened->Read(actual.id,0,0,data,sizeof(data));reopened->Read(actual.id,1,0,resource,sizeof(resource));
   reopened->Close();
   Check(!memcmp(data,"EDIT",4)&&!memcmp(resource,"RSR",3),"launcher wrote the wrong data or lost the resource fork");
   fprintf(report,"PASS: launcher write-back preserves the resource fork and Finder metadata, and retains edits when the original has changed.\n");
  }
  {
   const auto folder=W(root)+L"\\\u65e5\u672c";
   const auto original=folder+L"\\\u6587\u4ef6.txt";
   const auto renamed=folder+L"\\\u65b0.txt";
   Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"create Unicode mutation folder");
   auto file=CreateFileW(original.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
   Check(file!=INVALID_HANDLE_VALUE,"create Unicode mutation file");CloseHandle(file);
   auto location=Location::Windows(original);
   location.expectedIdentity=MacBrowser::DescribeWindowsFile(original).identity;
   Pass(Submit(RenameEntry{location,u"\u65b0.txt"}));
   Check(GetFileAttributesW(original.c_str())==INVALID_FILE_ATTRIBUTES&&
         GetFileAttributesW(renamed.c_str())!=INVALID_FILE_ATTRIBUTES,"rename lost the Unicode path");
   auto target=Location::Windows(folder);target.directory=true;
   target.expectedIdentity=MacBrowser::DescribeWindowsFile(folder).identity;
   Pass(Submit(DeleteEntries{{target}}));
   Check(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES,"recursive deletion lost a Unicode child");
   fprintf(report,"PASS: typed Windows rename and recursive delete preserve Unicode paths.\n");
  }
  {
   auto host=OpenWindows(W(root));
   Put(*host,u"Companion identity");
   const auto file=root+"\\Companion identity";
   const auto sidecar=W(root+"\\%Companion identity");
   auto selected=Location::Windows(W(file));
   selected.expectedIdentity=DescribeWindowsFile(W(file)).identity;
   Check(!selected.expectedIdentity.companion.key.empty(),"Capture Windows companion identity");
   auto handle=CreateFileW(sidecar.c_str(),FILE_WRITE_ATTRIBUTES|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,
                           nullptr,OPEN_EXISTING,0,nullptr);
   Check(handle!=INVALID_HANDLE_VALUE,"Open companion for version change");
   FILETIME modified{};
   const bool changed=GetFileTime(handle,nullptr,nullptr,&modified)!=FALSE;
   ++modified.dwHighDateTime;
   const bool saved=changed&&SetFileTime(handle,nullptr,nullptr,&modified)!=FALSE;
   CloseHandle(handle);
   const auto updated=DescribeWindowsFile(W(file)).identity;
   Check(saved&&updated.SameObject(selected.expectedIdentity)&&updated.primary==selected.expectedIdentity.primary&&
         updated.companion!=selected.expectedIdentity.companion,"Companion update changed the primary identity");
   auto stale=Submit(RenameEntry{selected,u"Unexpected rename"});
   Wait(stale);
   Check(stale.Job()->result().code&&GetFileAttributesW(sidecar.c_str())!=INVALID_FILE_ATTRIBUTES,
         "Stale companion selection modified the source");
   selected.expectedIdentity=updated;
   Check(MoveFileA(file.c_str(),(file+"-old").c_str())!=FALSE,"Retain original host file");
   Save(file,"Replacement");
   auto replacement=Submit(RenameEntry{selected,u"Unexpected replacement rename"});
   Wait(replacement);
   Check(replacement.Job()->result().code&&Load(file)==std::vector<BYTE>({'R','e','p','l','a','c','e','m','e','n','t'}),
         "Captured object identity accepted a replacement at the same path");
   fprintf(report,"PASS: stale Windows companion and same-path replacement identities refused.\n");
  }
  WindowsStagingSafety(root,report);
  Request classicRename = FileOperations::RenameEntry{classicSource, u"Renamed"};Pass(Submit(classicRename));
  Request legacy;legacy = FileOperations::LegacyConversion{};legacy.Legacy().source=FileOperations::Location::FromLegacy(classic,"Classic:Renamed");legacy.Legacy().destination=FileOperations::Location::FromLegacy(root,root+"\\raw.dat");legacy.Legacy().options.output='r';legacy.Legacy().options.input='r';legacy.Legacy().options.appledouble=false;
  {Gate gate;auto captured=Submit(legacy);legacy.Legacy().options.output='5';HFSIFACE_restore_copy_settings('5','m',1);gate.Release();Pass(captured);char out,in;int ad;HFSIFACE_copy_settings(&out,&in,&ad);Check(out=='5'&&in=='m'&&ad==1&&Load(root+"\\raw.dat")==std::vector<BYTE>({'D','A','T','A'}),"queued conversion settings were not captured/restored");}
  {
   const auto folder=root+"\\launcher-export";
   Check(CreateDirectoryA(folder.c_str(),nullptr)!=FALSE,"create launcher staging fixture");
   Save(folder+"\\Decoy","UNRELATED");
   auto conversion=legacy;
   conversion.Legacy().destination=Location::Windows(W(folder));
   conversion.Legacy().target=ConversionTarget::Folder;
   conversion.Legacy().options.output='b';
   auto exported=Submit(conversion);
   Pass(exported);
   const auto publication=exported.Snapshot();
   Check(publication.publishedWindowsExport==BrowserPresentation::WindowsPath(W(folder+"\\Renamed.hqx"))&&!Load(folder+"\\Renamed.hqx").empty(),
         "worker did not report the actual launcher export");
   const auto path=W(folder+"\\Renamed.hqx"),moved=path+L".original";
   Check(MoveFileW(path.c_str(),moved.c_str())!=FALSE,"replace a published launcher export");
   Save(folder+"\\Renamed.hqx","DECOY");
   {
    LauncherHandoff handoff;
    Check(handoff.Protect(path.c_str(),publication.publishedWindowsIdentity,true).code&&
          !(GetFileAttributesW(path.c_str())&(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_TEMPORARY)),
          "launcher accepted or modified a substituted file");
   }
   Check(DeleteFileW(path.c_str())&&MoveFileW(moved.c_str(),path.c_str()),"restore launcher export fixture");
   {
    LauncherHandoff handoff;
    Check(!handoff.Protect(path.c_str(),publication.publishedWindowsIdentity,false).code,
          "launcher refused its published file");
    Check(!MoveFileW(path.c_str(),moved.c_str())&&
          (GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_TEMPORARY),
          "launcher failed to pin its staged pathname while applying attributes");
    LauncherTemporaryDocument read;
    Check(!read.Open(path.c_str(),false).code,"handoff pin blocked baseline capture");
   }
   auto skipped=Submit(conversion);
   Conflict(skipped.Job());
   skipped.Job()->resolve(Storage::ConflictChoice::Skip);
   Wait(skipped);
   Check(!skipped.Job()->result().code&&!skipped.Snapshot().complete&&
         skipped.Snapshot().publishedWindowsExport.empty()&&
         Load(folder+"\\Decoy")==std::vector<BYTE>({'U','N','R','E','L','A','T','E','D'}),
         "skipped launcher export reported an existing file as published");
   fprintf(report,"PASS: launcher conversion reports its exact published file and reports no file on Skip.\n");
  }
  Save(root+"\\import.dat","HELLO");legacy.Legacy().source=FileOperations::Location::FromLegacy(root,root+"\\import.dat");legacy.Legacy().destination=FileOperations::Location::FromLegacy(classic,"Classic:Imported");legacy.Legacy().options.input='r';Pass(Submit(legacy));hfsdirent imported{};std::string importPath="Classic:Imported";Check(HFSIFACE_get_properties(&classic[0],&importPath[0],&imported)==0&&imported.u.file.dsize==5,"captured import mode ignored");
  classicSource.SetClassic(classicSource.ClassicImage(),"Classic:Renamed");Request classicRemove=DeleteEntries{{classicSource}};Pass(Submit(classicRemove));
  uint32_t moving=Put(*source,u"Move me"),moveFolder=target->MakeDirectory(2,u"Moves");auto movingEntry=source->Stat(moving);Request move;move.Copy().sources={Native(source,moving,&movingEntry)};move.Copy().sources[0].cut=true;move.Copy().destination=Native(target,moveFolder);auto moved=Submit(move);Pass(moved);bool removed=false;try{source->Stat(moving);}catch(const std::exception&){removed=true;}Check(removed&&moved.Job()->result().moved&&moved.Snapshot().affected.size()==2&&target->List(moveFolder).size()==1,"successful move did not finalize destination and remove source");
  uint32_t kept=Put(*source,u"Payload");move.Copy().sources={Native(source,kept)};move.Copy().sources[0].cut=true;move.Copy().destination=Native(target,folder);auto skippedMove=Submit(move);Conflict(skippedMove.Job());skippedMove.Job()->resolve(Storage::ConflictChoice::Skip);Wait(skippedMove);Check(!skippedMove.Job()->result().moved&&!skippedMove.Snapshot().complete&&source->Stat(kept).name==u"Payload","skipped move removed source");
  {auto mounted=hfs_mount(&classic[0],0,O_RDWR);char dir[]="Classic:Cancel",child[]="Classic:Cancel:Keep";Check(mounted&&hfs_mkdir(mounted,dir)==0&&hfs_create(mounted,child,type,creator)==0,"classic delete cancellation fixture");unsigned calls=0;HFSIFACE_delete_cancel([](void* p)->int{return ++*static_cast<unsigned*>(p)>=2;},&calls);int cancelled=do_delete_directory(mounted,dir);HFSIFACE_delete_cancel(nullptr,nullptr);hfsdirent entry{};Check(cancelled!=0&&hfs_stat(mounted,child,&entry)==0&&hfs_umount(mounted)==0,"classic recursive-delete cancellation lost unvisited children");}
  classicSource.SetClassic(classicSource.ClassicImage(),"Classic:Cancel");classicSource.directory=true;classicRemove=DeleteEntries{{classicSource}};Pass(Submit(classicRemove));
#ifdef HFV_FLOPPY_TESTING
  {std::string fixture=root+"\\floppy.dsk";{CFile disk;Check(disk.Open(fixture.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create simulated floppy");disk.SetLength(1474560);}char label[]="Floppy",old[]="Floppy:Before";Check(hfs_format(&fixture[0],0,label)==0,"format simulated floppy");auto volume=hfs_mount(&fixture[0],0,O_RDWR);Check(volume&&hfs_create(volume,old,type,creator)==0&&hfs_umount(volume)==0,"populate simulated floppy");
   OperationFloppy disk(Load(fixture));readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach simulated operation floppy");struct Detach{~Detach(){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}}detach;
   char path[64];get_floppy_volume_file_name(25,path);Location entry=Location::Classic(path,old);Request ren=RenameEntry{entry,u"After"};ren.background=true;auto before=disk.disk.media;disk.denyLock=true;auto denied=Submit(ren);Wait(denied);Check(denied.Job()->result().error==ERROR_ACCESS_DENIED&&disk.writes==0&&disk.disk.media==before,"floppy mutation lock failure changed media");disk.denyLock=false;Pass(Submit(ren));Check(disk.writes>0&&!disk.locked,"floppy rename did not finalize verified writes");
   entry.SetClassic(entry.ClassicImage(),"Floppy:After");Request del=DeleteEntries{{entry}};del.background=true;Pass(Submit(del));physical_floppy_suspend(25);volume=hfs_mount(path,0,O_RDONLY);hfsdirent gone{};std::string after="Floppy:After";Check(volume&&hfs_stat(volume,&after[0],&gone)<0&&hfs_umount(volume)==0&&!disk.locked,"floppy delete job did not persist");
   // The old converted PC->HFS route must ask before staging a replacement,
   // including when conversion turns a PC basename into a different Mac name.
   Save(root+"\\Duplicate","OLD");Request import;import = FileOperations::LegacyConversion{};import.Legacy().source=FileOperations::Location::FromLegacy(root,root+"\\Duplicate");import.Legacy().destination=FileOperations::Location::FromLegacy(path,"Floppy:");import.Legacy().options.output=import.Legacy().options.input='r';import.Legacy().options.appledouble=false;Pass(Submit(import));Save(root+"\\Duplicate","NEW DATA");
   auto readImported=[&](){physical_floppy_suspend(25);auto v=hfs_mount(path,0,O_RDONLY);Check(v!=nullptr,"read imported floppy file");char name[]="Floppy:Duplicate";auto f=hfs_open(v,name);Check(f!=nullptr,"open imported floppy file");char data[32]{};auto n=hfs_read(f,data,sizeof(data));Check(hfs_close(f)==0&&hfs_umount(v)==0,"close imported floppy file");return std::string(data,size_t(n));};
   for(auto choice:{Storage::ConflictChoice::Cancel,Storage::ConflictChoice::Skip}){auto overwrite=Submit(import);Conflict(overwrite.Job());overwrite.Job()->resolve(choice);Wait(overwrite);Check(readImported()=="OLD"&&Load(root+"\\Duplicate").size()==8,"declined floppy overwrite changed destination/source");if(choice==Storage::ConflictChoice::Cancel)Check(overwrite.Job()->result().error==ERROR_CANCELLED,"floppy Cancel not propagated");else Check(!overwrite.Job()->result().code&&!overwrite.Snapshot().complete&&overwrite.Job()->progress().skipped==1,"floppy Skip not propagated");}
   Request wrappedExport;wrappedExport = FileOperations::LegacyConversion{};wrappedExport.Legacy().source=FileOperations::Location::FromLegacy(path,"Floppy:Duplicate");wrappedExport.Legacy().destination=FileOperations::Location::FromLegacy(root,root+"\\container.bin");wrappedExport.Legacy().options.output='m';wrappedExport.Legacy().options.appledouble=false;Pass(Submit(wrappedExport));auto wrapped=import;wrapped.SetLegacySourcePath(wrappedExport.Legacy().destination.LegacyPath());wrapped.Legacy().options.input='m';auto embedded=Submit(wrapped);Conflict(embedded.Job());Check(embedded.Job()->conflict().find("Duplicate")!=std::string::npos,"overwrite checked container filename rather than embedded Mac name");embedded.Job()->resolve(Storage::ConflictChoice::Skip);Wait(embedded);Check(!embedded.Job()->result().code&&embedded.Job()->progress().skipped==1&&readImported()=="OLD","skipped converted-name import changed floppy data");
   auto replace=Submit(import);Conflict(replace.Job());replace.Job()->resolve(Storage::ConflictChoice::Replace);Pass(replace);Check(readImported()=="NEW DATA","confirmed floppy replacement not saved");
   const auto moveSource=root+"\\Converted move.txt";
   Save(moveSource,"first\r\nsecond\r\n");
   auto convertedMove=import;
   convertedMove.SetLegacySourcePath(moveSource);
   convertedMove.Legacy().options.input='t';
   convertedMove.Legacy().move=true;
   convertedMove.Legacy().destination.BindMediaGeneration(disk.disk.changeCount);
   auto unchangedDisk=disk.disk.media;
   disk.denyWrites=true;
   auto failedMove=Submit(convertedMove);Wait(failedMove);
   Check(failedMove.Job()->result().code&&!failedMove.Job()->result().moved&&
         GetFileAttributesA(moveSource.c_str())!=INVALID_FILE_ATTRIBUTES&&disk.disk.media==unchangedDisk&&!disk.locked,
         "failed converted floppy write removed its Cut source or changed untouched media");
   disk.denyWrites=false;
   auto movedConversion=Submit(convertedMove);Pass(movedConversion);
   Check(movedConversion.Job()->result().moved&&GetFileAttributesA(moveSource.c_str())==INVALID_FILE_ATTRIBUTES&&!disk.locked,
         "verified converted floppy write did not commit before removing its Cut source");
   fprintf(report,"PASS: typed text conversion retains a Cut source on floppy write failure and removes it only after verified retry.\n");
   auto launch=Location::Classic(path,"Floppy:Duplicate");
   launch.BindMediaGeneration(disk.disk.changeCount);
   Storage::Job capture;
   auto version=CaptureLaunchVersion(launch,capture);
   ++disk.disk.changeCount;
   auto writes=disk.writes;
   Storage::Job playback;
   LauncherTemporaryDocument edit;
   Check(!edit.Open(W(root+"\\Duplicate").c_str(),false).code,"protect floppy launcher edit");
   auto rejected=ApplyLaunchEdit(launch,version,edit.Handle(),playback);
   Check(rejected.error==ERROR_MEDIA_CHANGED&&disk.writes==writes&&!disk.locked&&
         Load(root+"\\Duplicate").size()==8,"launcher wrote to swapped floppy media or removed the edit");
   // Exercise the converted-move path without the physical-floppy wrapper's
   // extra source-retention policy: Skip must never fall through to DeleteFile.
   auto imageMove=import;imageMove.SetLegacySourcePath(root+"\\ImageMove");imageMove.Legacy().destination=FileOperations::Location::FromLegacy(classic,"Classic:");Save(imageMove.Legacy().source.LegacyPath(),"KEEP");Pass(Submit(imageMove));Save(imageMove.Legacy().source.LegacyPath(),"NEW");imageMove.Legacy().move=true;auto skippedImport=Submit(imageMove);Conflict(skippedImport.Job());skippedImport.Job()->resolve(Storage::ConflictChoice::Skip);Wait(skippedImport);Check(!skippedImport.Job()->result().code&&!skippedImport.Snapshot().complete&&Load(imageMove.Legacy().source.LegacyPath()).size()==3,"skipped converted move deleted its Windows source");auto unchanged=OpenClassic(classic,"Classic:",false);bool keptOld=false;for(auto& e:unchanged->List())if(e.name==u"ImageMove"){char data[4]{};unchanged->Read(e.id,0,0,data,4);keptOld=!memcmp(data,"KEEP",4);}unchanged->Close();Check(keptOld,"skipped converted move changed its old destination");
   fprintf(report,"PASS: simulated physical-floppy rename/delete and converted duplicate-file imports; exclusive locking, Cancel/Skip preserve old data, Replace publishes new data, verified/finalized writes and unsupported flush. No physical drive opened.\n");
  }
#endif
  Check(!delayed->wrongThread,"native mutation ran on UI thread");Check(Pending().empty(),"completed operations left in pending registry");
  fprintf(report,"PASS: real journaled HFS+/HFSX operations, legacy Windows and classic HFS rename/delete, retained raw/BinHex conversion route and captured import/export/AppleDouble settings restored after queued operations, successful/skipped moves and cancellation between classic delete entries. No physical devices or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());CancelPending();result=1;}
 app.m_pMainWnd=nullptr;if(owner.m_hWnd)owner.DestroyWindow();if(other.m_hWnd)other.DestroyWindow();fclose(report);return result;
}
#endif
