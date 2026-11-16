// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "FileOperations.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
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
 std::shared_ptr<Volume> inner;DWORD ui=GetCurrentThreadId();unsigned delay=0;bool failRead=false,wrongThread=false;uint32_t failRemove=0;CWnd* owner=nullptr;CWnd* other=nullptr;bool onlyOwnerDisabled=false;
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
 void Remove(uint32_t id)override{Thread();if(failRemove==id)throw HfsPlus::Error("Injected delete error");inner->Remove(id);}
};
uint32_t Put(Volume& volume,const std::u16string& name){Entry e;e.name=name;e.forks[0].size=4096;e.forks[1].size=17;return volume.Put(2,e,[](unsigned f,uint64_t at,void* p,size_t n){auto bytes=(BYTE*)p;for(size_t i=0;i<n;++i)bytes[i]=BYTE(at+i+f*31);},false);}
Location Native(const std::shared_ptr<Volume>& volume,uint32_t id,const Entry* expected=nullptr){Location out;out.type=LIST_MAC;out.volume=volume;out.id=id;if(expected){out.expectedIdentity=expected->identity;out.expectedName=expected->name;out.expectedParent=expected->parent;}return out;}
Location Host(const std::string& path,bool directory=false){Location out;out.type=LIST_FAT;out.windowsPath=W(path);out.directory=directory;return out;}
void Wait(const Operation& op){Check(WaitForSingleObject(op.Job()->event(),15000)==WAIT_OBJECT_0,"operation timed out");}
void Pass(const Operation& op){Wait(op);if(op.Job()->result().code)throw HfsPlus::Error(op.Job()->result().message);Check(op.Snapshot().complete,"operation incomplete");}
void Conflict(const std::shared_ptr<Storage::Job>& job){DWORD until=GetTickCount()+5000;while(!job->done()&&job->conflict().empty()&&LONG(until-GetTickCount())>0)Sleep(10);Check(!job->conflict().empty(),"expected overwrite conflict");}
std::vector<BYTE> Load(const std::string& path){CFile file;Check(file.Open(path.c_str(),CFile::modeRead|CFile::shareDenyNone)!=FALSE,"read fixture");std::vector<BYTE> bytes(size_t(file.GetLength()));Check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"read fixture bytes");return bytes;}
void Save(const std::string& path,const char* data){CFile file;Check(file.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"write fixture");file.Write(data,UINT(strlen(data)));}
#ifdef HFV_FLOPPY_TESTING
class OperationFloppy:public FloppyWriteApi {
public:
 FakeFloppy disk;bool locked=false,denyLock=false;unsigned writes=0;
 explicit OperationFloppy(const std::vector<BYTE>& data):disk(data){}
 UINT DriveType(int d)override{return disk.DriveType(d);}HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{return disk.Open(p,a,s,f);}
 bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk.Geometry(h,g);}bool Verify(HANDLE h,DWORD& n)override{return disk.Verify(h,n);}bool Read(HANDLE h,ULONGLONG at,DWORD n,void* p,DWORD& got)override{return disk.Read(h,at,n,p,got);}void Close(HANDLE h)override{disk.Close(h);}
 bool Lock(HANDLE)override{if(denyLock){SetLastError(ERROR_ACCESS_DENIED);return false;}locked=true;return true;}bool Dismount(HANDLE)override{return locked;}bool Unlock(HANDLE)override{locked=false;return true;}
 bool FormatTracks1440(HANDLE)override{throw HfsPlus::Error("Mutation must not format a floppy");}
 bool Write(HANDLE,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{Check(locked&&at<=disk.media.size()&&n<=disk.media.size()-at,"unsafe simulated floppy write");memcpy(disk.media.data()+size_t(at),p,n);got=n;++writes;return true;}
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
  Check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden operation owner",WS_OVERLAPPED,0,0,600,400,nullptr,0)&&other.CreateEx(0,AfxRegisterWndClass(0),"Other browser",WS_OVERLAPPED,0,0,600,400,nullptr,0),"create hidden owners");app.m_pMainWnd=&other;app.m_mac_window_mode=0;app.m_pre=nullptr;
  auto source=Image(W(root+"\\source.dsk"),u"Source",HfsPlus::Volume::FormatKind::HfsPlus),target=Image(W(root+"\\target.dsk"),u"Target",HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
  uint32_t file=Put(*source,u"Payload"),folder=target->MakeDirectory(2,u"Copies");auto expected=source->Stat(file);auto delayed=std::make_shared<Delayed>(source);delayed->delay=120;delayed->owner=&owner;delayed->other=&other;
  Request copy;copy.sources={Native(delayed,file,&expected)};copy.target=Native(target,folder);
  // Destroy the originating session and its panes while its captured request waits.
  std::weak_ptr<Volume> retained=delayed;Operation detached=[&]{Gate gate;Tree tree;List list;Operation operation=[&]{BrowserSession session;session.Attach(&owner,&tree,&list);Check(tree.Create(nullptr,nullptr,WS_CHILD,CRect(0,0,200,300),&owner,100,nullptr)&&list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,600,300),&owner,101,nullptr),"create originating panes");tree.Init();list.Initialize();session.MacBrowser().AttachTestVolume(delayed,"source");auto op=Submit(copy);copy.sources[0].id=0xffffffff;copy.target.id=0xffffffff;return op;}();list.DestroyWindow();tree.DestroyWindow();delayed.reset();copy.sources.clear();gate.Release();return operation;}();
  Pass(detached);auto copied=target->List(folder);Check(copied.size()==1&&copied[0].name==u"Payload"&&copied[0].forks[1].size==17,"captured copy request changed with caller state");Storage::worker().submit([](Storage::Job&){return Storage::Result{};})->wait();Check(retained.expired(),"completed operation retained its source backend through history");
  fprintf(report,"PASS: copy request survives originating session/pane destruction, retains backend lifetime until execution, owns original IDs/options, and releases backend references after completion.\n");
  delayed=std::make_shared<Delayed>(source);delayed->delay=120;delayed->owner=&owner;delayed->other=&other;Request shown;shown.sources={Native(delayed,file,&expected)};shown.target=Native(target,2);auto visible=Submit(shown);storageUiTest={};auto shownResult=ShowSubmittedStorageJob(visible.Job(),"Copying",true,true,true,&owner);Check(!shownResult.code&&visible.Snapshot().complete&&visible.Job()->progress().processed==visible.Job()->progress().totalBytes&&storageUiTest.timerTicks>0&&storageUiTest.copyBarCount==1&&delayed->onlyOwnerDisabled&&owner.IsWindowEnabled()&&other.IsWindowEnabled()&&!delayed->wrongThread,"submitted-job presentation used another owner or blocked UI");
  {Request layout=shown;layout.target=Native(target,target->MakeDirectory(2,u"Folder layout"));auto op=Submit(layout);storageUiTest={};auto value=ShowSubmittedStorageJob(op.Job(),"Copying folder",true,false,true,&owner);Check(!value.code&&storageUiTest.copyBarCount==2,"submitted folder copy lost overall/file progress bars");}
  {Request cancel=shown;cancel.target=Native(target,target->MakeDirectory(2,u"Cancel layout"));auto op=Submit(cancel);storageUiTest={};storageUiTest.cancelOnTimer=true;auto value=ShowSubmittedStorageJob(op.Job(),"Cancelling copy",true,true,true,&owner);Check(value.error==ERROR_CANCELLED&&owner.IsWindowEnabled()&&other.IsWindowEnabled(),"submitted-job Cancel button or owner restoration");storageUiTest={};}
  Request skip=shown;auto skipped=Submit(skip);Conflict(skipped.Job());skipped.Job()->resolve(Storage::ConflictChoice::Skip);Wait(skipped);Check(!skipped.Job()->result().code&&!skipped.Snapshot().complete&&skipped.Job()->progress().skipped==1,"skipped copy reported fully complete");
  std::shared_ptr<Storage::Job> orphan;{auto abandoned=Submit(shown);orphan=abandoned.Job();}Conflict(orphan);auto pending=Pending();Check(std::any_of(pending.begin(),pending.end(),[&](const Operation& op){return op.Job()==orphan;}),"detached conflict absent from application operation registry");CancelPending();orphan->resolve(Storage::ConflictChoice::Replace);orphan->wait();Check(orphan->result().error==ERROR_CANCELLED&&source->Stat(file).name==u"Payload","detached conflict cancellation lost source");pending.clear();
  {Gate gate;Request remove;remove.kind=Kind::Delete;remove.sources={Native(delayed,file,&expected)};auto cancelled=Submit(remove);cancelled.Job()->cancel();gate.Release();Wait(cancelled);Check(cancelled.Job()->result().error==ERROR_CANCELLED&&cancelled.Snapshot().affected.empty()&&source->Stat(file).name==u"Payload","queued cancellation mutated source");}
  Request rename;rename.kind=Kind::Rename;rename.sources={Native(delayed,file,&expected)};rename.target=Native(delayed,2);rename.name=u"Renamed";
  {Gate gate;auto first=Submit(rename);auto stale=Submit(rename);rename.name=u"Changed after submission";gate.Release();Pass(first);Wait(stale);Check(stale.Job()->result().code&&source->Stat(file).name==u"Renamed","stale queued rename operated on changed selection");}
  expected=source->Stat(file);delayed->failRead=true;Request bad;bad.sources={Native(delayed,file,&expected)};bad.target=Native(target,folder);bad.sources[0].cut=true;auto failed=Submit(bad);Wait(failed);Check(failed.Job()->result().code&&!failed.Snapshot().complete&&!failed.Snapshot().affected.empty()&&source->Stat(file).name==u"Renamed","copy failure lost cut source or omitted affected resource");delayed->failRead=false;delayed->delay=0;
  uint32_t one=Put(*source,u"Delete one"),two=Put(*source,u"Delete two");delayed->failRemove=two;Request remove;remove.kind=Kind::Delete;remove.sources={Native(delayed,one),Native(delayed,two)};auto partial=Submit(remove);Wait(partial);Check(partial.Job()->result().code&&!partial.Snapshot().complete&&partial.Snapshot().completed==1&&source->Stat(two).name==u"Delete two","partial delete completion was lost");delayed->failRemove=0;
  Request raw;raw.kind=Kind::ExportFork;raw.sources={Native(delayed,file,&expected)};raw.target=Host(root+"\\resource.bin");raw.fork=1;Pass(Submit(raw));Check(Load(root+"\\resource.bin").size()==17,"raw fork export job");auto repeated=Submit(raw);Wait(repeated);Check(repeated.Job()->result().code&&Load(root+"\\resource.bin").size()==17,"fork export overwrote existing output");
  Save(root+"\\replacement.bin","NEW RESOURCE");Request resource;resource.kind=Kind::ReplaceResource;resource.sources={Host(root+"\\replacement.bin")};resource.target=Native(delayed,file,&expected);Pass(Submit(resource));Check(source->Stat(file).forks[0].size==4096&&source->Stat(file).forks[1].size==12,"resource import job lost data fork");
  fprintf(report,"PASS: worker-only copy/rename/delete and fork jobs, slow responsive owner-specific progress, overwrite skip, detached conflict discovery/cancellation, queued cancellation, stale rename rejection, cut-source retention, partial-delete accounting and raw-export overwrite preservation.\n");
  // The legacy backend keeps its safe path-based host mutations and converters.
  Save(root+"\\ordinary.txt","KEEP");Save(root+"\\%ordinary.txt","UNRELATED");Request hostRename;hostRename.kind=Kind::Rename;hostRename.sources={Host(root+"\\ordinary.txt")};hostRename.name=u"renamed.txt";Pass(Submit(hostRename));Check(Load(root+"\\renamed.txt").size()==4,"Windows rename job");Request hostRemove;hostRemove.kind=Kind::Delete;hostRemove.sources={Host(root+"\\renamed.txt")};Pass(Submit(hostRemove));Check(Load(root+"\\%ordinary.txt").size()==9,"Windows delete removed unrelated companion");
  std::string classic=root+"\\classic.dsk";{CFile f;Check(f.Open(classic.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic job image");f.SetLength(2*1024*1024);}char label[]="Classic",type[]="TEXT",creator[]="TEST",name[]="Classic:Original";Check(hfs_format(&classic[0],0,label)==0,"format classic job fixture");auto v=hfs_mount(&classic[0],0,O_RDWR);Check(v&&hfs_create(v,name,type,creator)==0,"create classic job source");auto f=hfs_open(v,name);Check(f&&hfs_write(f,const_cast<char*>("DATA"),4)==4&&hfs_close(f)==0&&hfs_umount(v)==0,"write classic source");
  Location classicSource;classicSource.type=LIST_HFS;classicSource.classicImage=classic;classicSource.classicPath="Classic:Original";Request classicRename;classicRename.kind=Kind::Rename;classicRename.sources={classicSource};classicRename.name=u"Renamed";Pass(Submit(classicRename));
  Request legacy;legacy.kind=Kind::LegacyCopy;legacy.sourceVolume=classic;legacy.sourcePath="Classic:Renamed";legacy.targetVolume=root;legacy.targetPath=root+"\\raw.dat";legacy.conversion.output='r';legacy.conversion.input='r';legacy.conversion.appledouble=false;
  {Gate gate;auto captured=Submit(legacy);legacy.conversion.output='5';HFSIFACE_restore_copy_settings('5','m',1);gate.Release();Pass(captured);char out,in;int ad;HFSIFACE_copy_settings(&out,&in,&ad);Check(out=='5'&&in=='m'&&ad==1&&Load(root+"\\raw.dat")==std::vector<BYTE>({'D','A','T','A'}),"queued conversion settings were not captured/restored");}
  Save(root+"\\import.dat","HELLO");legacy.sourceVolume=root;legacy.sourcePath=root+"\\import.dat";legacy.targetVolume=classic;legacy.targetPath="Classic:Imported";legacy.conversion.input='r';Pass(Submit(legacy));hfsdirent imported{};std::string importPath="Classic:Imported";Check(HFSIFACE_get_properties(&classic[0],&importPath[0],&imported)==0&&imported.u.file.dsize==5,"captured import mode ignored");
  Request classicRemove;classicRemove.kind=Kind::Delete;classicSource.classicPath="Classic:Renamed";classicRemove.sources={classicSource};Pass(Submit(classicRemove));
  uint32_t moving=Put(*source,u"Move me"),moveFolder=target->MakeDirectory(2,u"Moves");auto movingEntry=source->Stat(moving);Request move;move.sources={Native(source,moving,&movingEntry)};move.sources[0].cut=true;move.target=Native(target,moveFolder);auto moved=Submit(move);Pass(moved);bool removed=false;try{source->Stat(moving);}catch(const std::exception&){removed=true;}Check(removed&&moved.Job()->result().moved&&moved.Snapshot().affected.size()==2&&target->List(moveFolder).size()==1,"successful move did not finalize destination and remove source");
  uint32_t kept=Put(*source,u"Payload");move.sources={Native(source,kept)};move.sources[0].cut=true;move.target=Native(target,folder);auto skippedMove=Submit(move);Conflict(skippedMove.Job());skippedMove.Job()->resolve(Storage::ConflictChoice::Skip);Wait(skippedMove);Check(!skippedMove.Job()->result().moved&&!skippedMove.Snapshot().complete&&source->Stat(kept).name==u"Payload","skipped move removed source");
  {auto mounted=hfs_mount(&classic[0],0,O_RDWR);char dir[]="Classic:Cancel",child[]="Classic:Cancel:Keep";Check(mounted&&hfs_mkdir(mounted,dir)==0&&hfs_create(mounted,child,type,creator)==0,"classic delete cancellation fixture");unsigned calls=0;HFSIFACE_delete_cancel([](void* p)->int{return ++*static_cast<unsigned*>(p)>=2;},&calls);int cancelled=do_delete_directory(mounted,dir);HFSIFACE_delete_cancel(nullptr,nullptr);hfsdirent entry{};Check(cancelled!=0&&hfs_stat(mounted,child,&entry)==0&&hfs_umount(mounted)==0,"classic recursive-delete cancellation lost unvisited children");}
  classicSource.classicPath="Classic:Cancel";classicSource.directory=true;classicRemove.sources={classicSource};Pass(Submit(classicRemove));
#ifdef HFV_FLOPPY_TESTING
  {std::string fixture=root+"\\floppy.dsk";{CFile disk;Check(disk.Open(fixture.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create simulated floppy");disk.SetLength(1474560);}char label[]="Floppy",old[]="Floppy:Before";Check(hfs_format(&fixture[0],0,label)==0,"format simulated floppy");auto volume=hfs_mount(&fixture[0],0,O_RDWR);Check(volume&&hfs_create(volume,old,type,creator)==0&&hfs_umount(volume)==0,"populate simulated floppy");
   OperationFloppy disk(Load(fixture));readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach simulated operation floppy");struct Detach{~Detach(){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}}detach;
   char path[64];get_floppy_volume_file_name(25,path);Location entry;entry.type=LIST_HFS;entry.classicImage=path;entry.classicPath=old;Request ren;ren.kind=Kind::Rename;ren.sources={entry};ren.name=u"After";auto before=disk.disk.media;disk.denyLock=true;auto denied=Submit(ren);Wait(denied);Check(denied.Job()->result().error==ERROR_ACCESS_DENIED&&disk.writes==0&&disk.disk.media==before,"floppy mutation lock failure changed media");disk.denyLock=false;Pass(Submit(ren));Check(disk.writes>0&&!disk.locked,"floppy rename did not finalize verified writes");
   Request del;del.kind=Kind::Delete;entry.classicPath="Floppy:After";del.sources={entry};Pass(Submit(del));physical_floppy_suspend(25);volume=hfs_mount(path,0,O_RDONLY);hfsdirent gone{};std::string after="Floppy:After";Check(volume&&hfs_stat(volume,&after[0],&gone)<0&&hfs_umount(volume)==0&&!disk.locked,"floppy delete job did not persist");
   fprintf(report,"PASS: simulated physical-floppy rename/delete jobs require an exclusive lock, reject lock failure without writes, verify/finalize changed data and tolerate unsupported flush. No physical drive opened.\n");
  }
#endif
  Check(!delayed->wrongThread,"native mutation ran on UI thread");Check(Pending().empty(),"completed operations left in pending registry");
  fprintf(report,"PASS: real journaled HFS+/HFSX operations, legacy Windows and classic HFS rename/delete, retained raw/BinHex conversion route and captured import/export/AppleDouble settings restored after queued operations, successful/skipped moves and cancellation between classic delete entries. No physical devices or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());CancelPending();result=1;}
 app.m_tree=nullptr;app.m_list=nullptr;app.m_pre=nullptr;app.m_pMainWnd=nullptr;if(owner.m_hWnd)owner.DestroyWindow();if(other.m_hWnd)other.DestroyWindow();fclose(report);return result;
}
#endif
