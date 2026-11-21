// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "BrowserPresentation.h"
#include "BackgroundCopies.h"
#include "Own_ids.h"
#include "BackgroundJobController.h"
#include "BrowserLifetime.h"
#include "DialogClose.h"
#include "MainMacBrowser.h"
#include "VolumeSession.h"
#include "MetadataDialogs.h"
#include "CommandStorage.h"
#include "MediaControl.h"
#include "CdImage.h"
#include "CdBurn.h"
#include "FloppyWriteDialog.h"
#include "ClassicFormatCommand.h"
#include "HfsPlusDialogs.h"
#include "FindFilesDialog.h"
#include "MediaCommands.h"
#include "RefreshStorage.h"
#include "StorageJobDialog.h"
#include "HfsPlusDisk.h"
#include "HfsPlusTime.h"
#include "BrowserDirectory.h"
#include "DeviceWait.h"
#include "utils.h"
#include "FloppyWriter.h"
#include "DirectFloppy.h"
#include "TransferFixture.h"
#include "LegacyEngine.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
#include "hfs/binhex.h"
int b_begin_mutation(hfsvol*);
int b_end_mutation(hfsvol*,int);
}
#include "FakeFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "floppy.h"
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <atomic>
#include "TaskbarProgress.h"
#include <cstdio>
namespace {
CString host,startupImage;
const char* testStage="initial";
bool Junction(const CString& link,const CString& target){
 if(!CreateDirectoryA(link,nullptr))return false;HANDLE h=CreateFileA(link,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
 CStringW wide(target);std::wstring print(wide.GetString()),substitute=L"\\??\\"+print;
 struct Buffer{DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;wchar_t path[1024];}data{};
 data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subLength=WORD(substitute.size()*2);data.printOffset=WORD(data.subLength+2);data.printLength=WORD(print.size()*2);
 memcpy(data.path,substitute.c_str(),data.subLength+2);memcpy(reinterpret_cast<BYTE*>(data.path)+data.printOffset,print.c_str(),data.printLength+2);data.length=WORD(8+data.printOffset+data.printLength+2);DWORD returned;
 bool ok=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&returned,nullptr)!=FALSE;CloseHandle(h);return ok;
}
void Check(bool ok,const char* text){if(!ok)throw HfsPlus::Error(text);}
void Pump(){MSG m;while(PeekMessage(&m,nullptr,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessage(&m);}}
template<class F>void Until(F ready){DWORD end=GetTickCount()+15000;while(!ready()&&LONG(end-GetTickCount())>0){Pump();Sleep(5);}Pump();Check(ready(),(std::string("background copy timed out: ")+testStage).c_str());}
class CopyDoc:public CHFVExplorerDoc{
 DECLARE_DYNCREATE(CopyDoc)
public:CopyDoc():CHFVExplorerDoc(false){Mounts().AddWindows(host[0],DRIVE_FIXED,host);if(!startupImage.IsEmpty())insert_volume(startupImage,0,0,0,0);}
};
IMPLEMENT_DYNCREATE(CopyDoc,CHFVExplorerDoc)
class CopyFrame:public CMainFrame{
 DECLARE_DYNCREATE(CopyFrame)
public:void LoadWindowPosition()override{}void SaveWindowPosition()override{}void ActivateFrame(int)override{}
};
IMPLEMENT_DYNCREATE(CopyFrame,CMainFrame)
int Row(BrowserSession& s,const wchar_t* name){auto& l=s.List()->GetListCtrl();for(int i=0;i<l.GetItemCount();++i){wchar_t text[512]{};LVITEMW v{};v.pszText=text;v.cchTextMax=ARRAYSIZE(text);SendMessageW(l.m_hWnd,LVM_GETITEMTEXTW,i,reinterpret_cast<LPARAM>(&v));if(!wcscmp(text,name))return i;}return -1;}
void Select(BrowserSession& s,const wchar_t* name){int row=Row(s,name);Check(row>=0,"copy fixture row missing");auto& l=s.List()->GetListCtrl();l.SetItemState(-1,0,LVIS_SELECTED);l.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}
std::shared_ptr<MacBrowser::Volume> Image(const CString& path){auto w=VolumeCoord::Wide((LPCSTR)path);auto disk=HfsPlus::CreateImage(w.c_str(),8*1024*1024);HfsPlus::Volume::Format(*disk,u"Background",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);disk.reset();return MacBrowser::OpenImage(w);}
uint32_t Put(MacBrowser::Volume& v,const std::u16string& name){MacBrowser::Entry e;e.name=name;e.forks[0].size=8192;e.forks[1].size=31;e.finder[0]='T';return v.Put(2,e,[](unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE(f+at+i);},false);}
uint32_t Find(MacBrowser::Volume& v,const std::u16string& name){for(auto& e:v.List())if(e.name==name)return e.id;return 0;}
struct Gate {
 HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr),release=CreateEvent(nullptr,TRUE,FALSE,nullptr);bool fail=false;
 ~Gate(){SetEvent(release);CloseHandle(entered);CloseHandle(release);}
 void Reset(){ResetEvent(entered);ResetEvent(release);fail=false;}
};
class Slow:public MacBrowser::Volume{
 std::shared_ptr<MacBrowser::Volume> base_;Gate& gate_;
public:Slow(std::shared_ptr<MacBrowser::Volume> base,Gate& gate):base_(base),gate_(gate){}
 std::string ResourceKey()const override{return base_->ResourceKey();}std::u16string Name()const override{return base_->Name();}const char* Kind()const override{return base_->Kind();}bool Writable()const override{return base_->Writable();}bool CaseSensitive()const override{return base_->CaseSensitive();}uint64_t FreeBytes()const override{return base_->FreeBytes();}
 MacBrowser::Entry Stat(uint32_t id)override{return base_->Stat(id);}std::vector<MacBrowser::Entry> List(uint32_t id)override{return base_->List(id);}void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{if(n){SetEvent(gate_.entered);Check(WaitForSingleObject(gate_.release,30000)==WAIT_OBJECT_0,"slow read was never released");if(gate_.fail)throw HfsPlus::Error("Injected slow-read failure");}base_->Read(id,f,at,p,n);}
 uint32_t MakeDirectory(uint32_t id,const std::u16string& n)override{return base_->MakeDirectory(id,n);}uint32_t Put(uint32_t id,const MacBrowser::Entry& e,MacBrowser::Reader r,bool replace)override{return base_->Put(id,e,r,replace);}void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{base_->Rename(id,parent,name);}void Remove(uint32_t id)override{base_->Remove(id);}void ValidateRemoval(uint32_t id)override{base_->ValidateRemoval(id);}
};
#ifdef HFV_FLOPPY_TESTING
class TestFloppy:public FloppyWriteApi {
public:FakeFloppy disk;bool locked=false;unsigned writes=0;
 explicit TestFloppy(std::vector<BYTE> bytes):disk(bytes){}
 UINT DriveType(int d)override{return disk.DriveType(d);}HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{return disk.Open(p,a,s,f);}
 bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk.Geometry(h,g);}bool Verify(HANDLE h,DWORD& n)override{return disk.Verify(h,n);}bool Read(HANDLE h,ULONGLONG at,DWORD n,void* p,DWORD& got)override{return disk.Read(h,at,n,p,got);}void Close(HANDLE h)override{disk.Close(h);}
 bool Lock(HANDLE)override{locked=true;return true;}bool Dismount(HANDLE)override{return locked;}bool Unlock(HANDLE)override{locked=false;return true;}
 bool FormatTracks1440(HANDLE)override{throw HfsPlus::Error("Copy must not format floppy");}
 bool Write(HANDLE,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{Check(locked&&at<=disk.media.size()&&n<=disk.media.size()-at,"unsafe floppy write");memcpy(disk.media.data()+size_t(at),p,n);got=n;++writes;return true;}
 bool Flush(HANDLE)override{SetLastError(ERROR_INVALID_FUNCTION);return false;}
};
#endif
void Host(BrowserSession& s,const CString& path){auto tree=s.Tree();bool enabled=tree->SelectionNotificationsEnabled();tree->enable_selchange(0);tree->GetTreeCtrl().SelectItem(s.MountRoot({BrowserMountInventory::Kind::WindowsDrive,0}));tree->enable_selchange(enabled);s.Document()->open_fat_directory(0,const_cast<char*>((LPCSTR)path),1);}
}
static void TestJobController(const std::wstring& directory)
{
 using Controller = BackgroundCopies::JobController;
 auto target = FileOperations::Location::Windows(directory);
 FileOperations::LegacyConversion conversion;
 conversion.destination = target;
 conversion.askMode = true;
 unsigned calls = 0;
 Controller planning({conversion}, [&](bool success) {
  ++calls;
  Check(!success, "cancelled copy-mode selection reported success");
 });
 Check(planning.Advance() == Controller::Step::ChooseMode && !planning.Job(), "controller submitted before mode selection");
 planning.Cancel();
 Check(planning.Advance() == Controller::Step::Finished, "cancelled planning still requested input");
 planning.Finish();
 planning.Finish();
 Check(calls == 1 && planning.Result().outcome == Storage::Outcome::Cancelled, "planning cancellation outcome or notification");

 FileOperations::CreateFolder create{target, u"controller-first"};
 FileOperations::CreateFolder duplicate{target, u"controller-first"};
 FileOperations::CreateFolder later{target, u"controller-later"};
 Controller sequence({create, duplicate, later}, [&](bool success) {
  ++calls;
  Check(!success, "failed batch reported success");
 });
 Check(sequence.Advance() == Controller::Step::Started, "controller failed to start first job");
 sequence.Job()->wait();
 Check(sequence.Advance() == Controller::Step::Started, "controller failed to start second job");
 sequence.Job()->wait();
 Check(sequence.Advance() == Controller::Step::Finished, "controller continued after failure");
 sequence.Finish();
 Check(calls == 2 && sequence.Result().outcome == Storage::Outcome::Failed &&
       GetFileAttributesW((directory + L"\\controller-first").c_str()) != INVALID_FILE_ATTRIBUTES &&
       GetFileAttributesW((directory + L"\\controller-later").c_str()) == INVALID_FILE_ATTRIBUTES,
       "controller did not retain earlier work or stop after failure");
}

int RunBackgroundCopiesTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\background-copy-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;app.m_copyin_mode=app.m_copyout_mode=0;
 Gate gate;
 try{
  TestJobController(VolumeCoord::Wide(directory));
  auto longFolder=VolumeCoord::Wide(std::string((LPCSTR)host));for(int level=0;level<3;++level){longFolder+=L"\\"+std::wstring(80,wchar_t(L'a'+level));auto full=BrowserPresentation::WindowsPath(longFolder);Check(CreateDirectoryW(full.c_str(),nullptr)!=FALSE,"create long source folder");}
  auto longFile=BrowserPresentation::WindowsPath(longFolder+L"\\StoredCase.txt");HANDLE file=CreateFileW(longFile.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);Check(file!=INVALID_HANDLE_VALUE,"create long source file");DWORD written;Check(WriteFile(file,"LONG",4,&written,nullptr)&&written==4,"write long source");CloseHandle(file);
  auto resolved=MacBrowser::ResolveWindowsFile(longFolder+L"\\storedcase.txt");Check(resolved.first->Stat(resolved.second).name==u"StoredCase.txt","direct long source lookup lost stored filename case");char longBytes[4]{};resolved.first->Read(resolved.second,0,0,longBytes,4);Check(!memcmp(longBytes,"LONG",4),"long source lookup read different data");resolved.first->Close();
  auto source=Image(host+"\\source.dsk"),target=Image(host+"\\target.dsk"),other=Image(host+"\\other.dsk");Put(*source,u"Payload");Put(*other,u"Unrelated");
  CString folder=host+"\\browse";Check(CreateDirectoryA(folder,nullptr)!=FALSE,"create browse folder");{CFile f;Check(f.Open(folder+"\\visible.txt",CFile::modeCreate|CFile::modeWrite),"create browse file");f.Write("browse",6);}
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);Check(a&&b,"create copy browsers");auto& s=a->GetBrowserSession();auto& second=b->GetBrowserSession();
  auto slow=std::make_shared<Slow>(source,gate);int src=s.MacBrowser().AttachTestVolume(slow,"slow-source"),dst=s.MacBrowser().AttachTestVolume(target,"target"),unrelated=s.MacBrowser().AttachTestVolume(other,"other");
  Check(s.MacBrowser().ShowTestFolder(src,2),"show copy source");Select(s,L"Payload");a->SendMessage(WM_COMMAND,ID_EDIT_COPY);Check(s.MacBrowser().ShowTestFolder(dst,2),"show destination");
  testStage="first slow copy";BackgroundCopies::enabled=true;storageUiTest={};a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);
  Check(BackgroundCopies::Active()&&a->IsWindowEnabled()&&!StorageUi::busy()&&!StorageUi::state().sessionOpen,"Paste blocked or disabled its browser");
  Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(!VolumeCoord::Readable(source->ResourceKey())&&!VolumeCoord::Readable(target->ResourceKey()),"copy did not reserve source and destination");
  bool busy=false;try{target->List();}catch(const VolumeCoord::Busy&){busy=true;}Check(busy,"unguarded backend read raced a writer");
  auto pending=FileOperations::Pending();Check(pending.size()==1,"background operation not retained");
  Check(s.MacBrowser().ShowTestFolder(dst,2)&&!s.ListingAvailable(),"busy navigation was not deferred");
  Check(s.MacBrowser().ShowTestFolder(unrelated,2)&&Row(s,L"Unrelated")>=0,"unrelated image could not be browsed during copying");
  Host(s,folder);Check(Row(s,L"visible.txt")>=0&&a->IsWindowEnabled()&&!pending.front().Job()->done(),"same-window host browsing waited for the copy");
  SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Check(pending.front().Snapshot().complete&&Find(*target,u"Payload"),"background copy failed");
  Check(Row(s,L"visible.txt")>=0,"stale deferred navigation replaced the newly selected folder");Check(BrowserTransfers::Any(),"Copy consumed its clipboard");
  auto copied=target->Stat(Find(*target,u"Payload"));Check(copied.forks[0].size==8192&&copied.forks[1].size==31&&copied.finder[0]=='T',"background copy lost fork metadata");BYTE bytes[31]{};target->Read(copied.id,1,0,bytes,sizeof(bytes));for(unsigned i=0;i<sizeof(bytes);++i)Check(bytes[i]==BYTE(i+1),"resource fork differs");
  // Pending replacement must not disable browsing; Skip retains Cut and source.
  testStage="overwrite skip";gate.Reset();Check(s.MacBrowser().ShowTestFolder(src,2),"show Cut source");Select(s,L"Payload");a->SendMessage(WM_COMMAND,ID_EDIT_CUT);Check(s.MacBrowser().ShowTestFolder(dst,2),"show replacement destination");storageUiTest={};a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);
  Until([]{auto p=FileOperations::Pending();return !p.empty()&&!p.front().Job()->conflict().empty();});Check(a->IsWindowEnabled()&&BackgroundCopies::DialogWindow(),"overwrite disabled browser or lost progress window");
  Host(s,folder);Check(Row(s,L"visible.txt")>=0,"overwrite prevented browsing");storageUiTest.answer=Storage::ConflictChoice::Skip;Until([]{return !BackgroundCopies::Active();});Check(Find(*source,u"Payload")&&BrowserTransfers::Snapshot().items.front().cut,"Skip deleted source or consumed Cut");
  // Successful replacement clears Cut only on completion; busy folder reload resumes.
  testStage="overwrite replace";gate.Reset();Check(s.MacBrowser().ShowTestFolder(dst,2),"show replace target");storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Replace;a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(BrowserTransfers::Any(),"Cut was cleared before commit");Check(s.MacBrowser().ShowTestFolder(dst,2),"defer destination view");SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Until([&]{return Row(s,L"Payload")>=0;});Check(!Find(*source,u"Payload")&&!BrowserTransfers::Any(),"successful Cut did not finalize source/clipboard");
  // Injected write-source read failure retains the old destination and moved source.
  testStage="read error";Put(*source,u"Failure");Put(*target,u"Failure");gate.Reset();gate.fail=true;Check(s.MacBrowser().ShowTestFolder(src,2),"show failing source");Select(s,L"Failure");a->SendMessage(WM_COMMAND,ID_EDIT_CUT);Check(s.MacBrowser().ShowTestFolder(dst,2),"show failing target");storageUiTest.answer=Storage::ConflictChoice::Replace;a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Check(Find(*source,u"Failure")&&Find(*target,u"Failure")&&BrowserTransfers::Any(),"failure lost source, old destination or Cut");Check(a->IsWindowEnabled(),"failure left browser disabled");
  // Closing a non-last frame leaves the job alive; explicit cancellation still
  // retains the source and discards its incomplete destination.
  testStage="cancel close";Put(*source,u"Cancel");gate.Reset();gate.fail=false;Check(s.MacBrowser().ShowTestFolder(src,2),"show cancel source");Select(s,L"Cancel");a->SendMessage(WM_COMMAND,ID_EDIT_CUT);Check(s.MacBrowser().ShowTestFolder(dst,2),"show cancel target");storageUiTest={};a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});HWND closing=a->GetSafeHwnd();a->SendMessage(WM_CLOSE);Check(!IsWindow(closing)&&b->IsWindowEnabled()&&BackgroundCopies::Active(),"window did not close independently or disabled another browser");BackgroundCopies::CancelAll();SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Until([&]{return !IsWindow(closing);});Check(Find(*source,u"Cancel")&&!Find(*target,u"Cancel"),"cancel lost source or published partial file");Check(VolumeCoord::Readable(source->ResourceKey())&&VolumeCoord::Readable(target->ResourceKey()),"cancel leaked volume reservation");
  Check(b->IsWindowEnabled(),"surviving browser disabled after close");
  // A second queued copy is independently owned and cancellable before it starts.
  auto enqueue=[&](uint32_t id){FileOperations::Request request;FileOperations::Location from,to;from.SetMounted(slow,id);to.SetMounted(target);request.Copy().sources={from};request.Copy().destination=to;return BackgroundCopies::Start({std::move(request)},b,true);};
  testStage="queue";auto firstId=Put(*source,u"QueuedOne"),secondId=Put(*source,u"QueuedTwo");gate.Reset();Check(enqueue(firstId),"start queued first");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});
  Check(enqueue(secondId)&&FileOperations::Pending().size()==2,"second job not queued independently");auto queue=FileOperations::Pending();queue.back().Job()->cancel();SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Check(Find(*target,u"QueuedOne")&&!Find(*target,u"QueuedTwo"),"queued cancellation wrote the canceled file");
#ifdef HFV_FLOPPY_TESTING
  {CString image=host+"\\floppy.dsk";{CFile file;Check(file.Open(image,CFile::modeCreate|CFile::modeWrite),"create fake floppy image");file.SetLength(1474560);}char label[]="TestFloppy";Check(hfs_format(const_cast<char*>((LPCSTR)image),0,label)==0,"format fake floppy image");std::vector<BYTE> bytes(1474560);{CFile file;Check(file.Open(image,CFile::modeRead),"load fake floppy");Check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"short fake floppy load");}
   TestFloppy disk(std::move(bytes));readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,&disk),"attach fake floppy");struct Detach{~Detach(){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}}detach;
   testStage="physical copy";char physical[MAX_PATH];get_floppy_volume_file_name(25,physical);auto doc=second.Document();int index=doc->insert_volume(physical,1,0,0,0);Check(index>=0&&doc->Mounts().OpenClassicForTest(index),"mount fake floppy browser");auto& mounted=doc->Mounts().ClassicAt(index);
   auto tree=second.Tree();doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},tree->initialize_volume(index,mounted.m_volume_name,2,1,0,0,0));auto root=doc->GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,index});bool notifications=tree->SelectionNotificationsEnabled();tree->enable_selchange(0);tree->GetTreeCtrl().SelectItem(root);tree->enable_selchange(notifications);Check(doc->NavigateClassicFolder(index,2)&&second.AwaitNavigationForTest(),"open fake floppy through typed navigation");
   auto copyFloppy=[&](const std::u16string& name){auto id=Put(*source,name);gate.Reset();FileOperations::Location from,to;from.SetMounted(slow,id);to.SetClassic(physical,"TestFloppy:");return second.MacBrowser().TransferSelection({from},to);};
   Check(copyFloppy(u"FloppyCopy"),"start fake floppy browser copy");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(disk.locked&&mounted.m_opened&&!VolumeCoord::Readable(VolumeCoord::DeviceKey(25)),"floppy write did not lock and reserve the device independently of browser mount metadata");
   SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Until([&]{return Row(second,L"FloppyCopy")>=0;});Check(mounted.m_opened&&!disk.locked&&disk.writes>0,"floppy browser did not automatically reopen/update");
   Check(copyFloppy(u"FloppyClick"),"start deferred floppy browser copy");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(doc->NavigateClassicFolder(index,2)&&Row(second,L"FloppyCopy")>=0&&!second.ListingAvailable(),"Mac floppy entry did not keep the previous listing during busy navigation");
   SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Until([&]{return Row(second,L"FloppyClick")>=0;});Check(mounted.m_opened,"deferred Macintosh entry did not reopen its reader");Check(second.WindowTitle()==L"TestFloppy \u2014 HFVExplorer","classic floppy title did not follow resumed navigation");
   doc->Mounts().CloseClassic(index);Host(second,folder);tree->GetTreeCtrl().SelectItem(root);Check(second.AwaitNavigationForTest()&&mounted.m_opened&&Row(second,L"FloppyCopy")>=0,"clicking Macintosh failed to reopen a closed reader without selecting drive root");
   // Conversion prompts own captured requests; navigation cannot retarget them.
   testStage="mode export";auto exportRequest=[&](const char* leaf){FileOperations::Request r;r = FileOperations::LegacyConversion{};r.Legacy().askMode=true;r.Legacy().source=FileOperations::Location::FromLegacy(physical,std::string("TestFloppy:")+leaf);r.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)folder,(LPCSTR)folder);return r;};
   Check(BackgroundCopies::Start({exportRequest("FloppyCopy"),exportRequest("FloppyClick")},b,true),"start modeless mode batch");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});Check(b->IsWindowEnabled()&&FileOperations::Pending().empty(),"mode picker disabled browsing or started copying before selection");Host(second,host);Check(Row(second,L"browse")>=0,"cannot browse while choosing copy mode");
   auto mode=BackgroundCopies::ModeWindow();SendDlgItemMessage(mode,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(mode,WM_COMMAND,IDOKALL,0);Until([]{return !BackgroundCopies::Active();});
   for(const char* leaf:{"FloppyCopy","FloppyClick"}){CFile exported;Check(exported.Open(folder+"\\"+leaf,CFile::modeRead)&&exported.GetLength()==8192,"modeless OK to all lost mode or captured destination");}
   auto cancelled=exportRequest("FloppyCopy");cancelled.SetLegacyTargetPath((LPCSTR)(host+"\\must-not-exist"));bool accepted=true;Check(BackgroundCopies::Start({cancelled},b,true,[&](bool ok){accepted=ok;}),"start cancel-mode prompt");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});SendMessage(BackgroundCopies::ModeWindow(),WM_CLOSE,0,0);Until([]{return !BackgroundCopies::Active();});Check(!accepted&&GetFileAttributesA(host+"\\must-not-exist")==INVALID_FILE_ATTRIBUTES,"canceling copy mode wrote files or reported completion");
   // The ordinary browser transfer captures an Ask-mode import without
   // running its old modal picker or changing the chosen Mac destination.
   Check(Junction(folder+"\\UnrelatedLink",host),"create unrelated sibling junction");bool refused=false;try{MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(std::string((LPCSTR)(folder+"\\UnrelatedLink"))));}catch(const HfsPlus::Error&){refused=true;}Check(refused,"selected junction was followed");
   testStage="mode import";app.m_copyin_mode=6;FileOperations::Location input,destination;input.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)(folder+"\\visible.txt"))));destination.SetClassic(physical,"TestFloppy:");
   Check(second.MacBrowser().TransferSelection({input},destination),"start browser Ask-mode import");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});Check(FileOperations::Pending().empty()&&b->IsWindowEnabled(),"browser Ask mode did not separate planning from copying");Host(second,folder);mode=BackgroundCopies::ModeWindow();SendDlgItemMessage(mode,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(mode,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});tree->GetTreeCtrl().SelectItem(root);Check(second.AwaitNavigationForTest()&&Row(second,L"visible.txt")>=0,"browser Ask-mode import used navigation instead of captured Mac destination");app.m_copyin_mode=0;
   FileOperations::Location exportedFile,windowsTarget;exportedFile.SetMounted(source,Put(*source,u"TargetLookup"));windowsTarget.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)folder)));Check(second.MacBrowser().TransferSelection({exportedFile},windowsTarget),"start export beside destination junction");Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&MacBrowser::DescribeWindowsFile(windowsTarget.WindowsPath()+L"\\TargetLookup").forks[1].size==31,"unrelated destination junction blocked generic export or lost resource fork");
   // Repeat the ordinary PC import against an existing floppy file. The
   // overwrite request must be presented before any replacement bytes are written.
   testStage="duplicate import";auto duplicate=[&](){app.m_copyin_mode=6;storageUiTest={};Check(second.MacBrowser().TransferSelection({input},destination),"start duplicate floppy import");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});auto picker=BackgroundCopies::ModeWindow();SendDlgItemMessage(picker,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(picker,WM_COMMAND,IDOK,0);
    Until([]{auto pending=FileOperations::Pending();return !BackgroundCopies::Active()||(!pending.empty()&&!pending.front().Job()->conflict().empty());});auto pending=FileOperations::Pending();Check(!pending.empty()&&!pending.front().Job()->conflict().empty(),("duplicate floppy import failed before overwrite prompt: "+BackgroundCopies::LastResult().message).c_str());Check(b->IsWindowEnabled(),"floppy overwrite disabled the browser");return pending.front().Job();};
   auto unchanged=disk.disk.media;auto writes=disk.writes;auto duplicateJob=duplicate();duplicateJob->resolve(Storage::ConflictChoice::Skip);Until([]{return !BackgroundCopies::Active();});Check(disk.disk.media==unchanged&&disk.writes==writes,"Skip changed floppy sectors");
   duplicateJob=duplicate();duplicateJob->resolve(Storage::ConflictChoice::Cancel);Until([]{return !BackgroundCopies::Active();});Check(disk.disk.media==unchanged&&disk.writes==writes,"Cancel changed floppy sectors");
   {CFile file;Check(file.Open(folder+"\\visible.txt",CFile::modeCreate|CFile::modeWrite),"change replacement source");file.Write("NEW CONTENT",11);}
   duplicateJob=duplicate();duplicateJob->resolve(Storage::ConflictChoice::Replace);Until([]{return !BackgroundCopies::Active();});Check(!duplicateJob->result().code&&disk.writes>writes,"Replace failed to write verified floppy changes");app.m_copyin_mode=0;

   physical_floppy_suspend(25);auto verify=MacBrowser::OpenClassic(physical,"TestFloppy:",false);auto file=Find(*verify,u"FloppyCopy");Check(file&&verify->Stat(file).forks[1].size==31,"fake floppy copy/fork verification failed");auto replaced=Find(*verify,u"visible.txt");char replacement[11]{};Check(replaced&&verify->Stat(replaced).forks[0].size==11,"replacement floppy length differs");verify->Read(replaced,0,0,replacement,sizeof(replacement));Check(!memcmp(replacement,"NEW CONTENT",11),"replacement floppy bytes differ");verify->Close();
   notifications=tree->SelectionNotificationsEnabled();tree->enable_selchange(0);tree->GetTreeCtrl().DeleteItem(root);tree->enable_selchange(notifications);doc->Mounts().RemoveClassic(index);

  }
#endif
  // File > Exit cancels every queued job and closes all frames after cleanup.
  testStage="shutdown";auto extra=app.CreateBrowser(FALSE);Check(extra!=nullptr,"create shutdown browser");Put(*source,u"Shutdown");gate.Reset();Check(enqueue(Find(*source,u"Shutdown")),"start shutdown copy");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});app.OnExitBrowsers();Check(app.m_browsers.size()==2,"Exit destroyed a browser during an active write");SetEvent(gate.release);Until([&]{return app.m_browsers.empty();});Check(!BackgroundCopies::Active()&&Find(*source,u"Shutdown")&&!Find(*target,u"Shutdown"),"Exit failed to cancel safely or close all browsers");
  fprintf(report,"PASS: non-modal Paste; same-window Windows and unrelated HFSX browsing during a gated slow read; source/destination reservations and guarded backend access; latest-navigation wins; deferred busy-volume reload; fork/Finder preservation; overwrite Skip/Replace while browsing; Cut finalization; injected read failure and replacement rollback; cancellation and safe frame close/lease release; independent queued cancellation; simulated floppy write/readback/unlock, automatic browser refresh, deferred Macintosh-entry selection and closed-reader reopening; modeless mode selection, captured destinations, OK to all and cancel; floppy overwrite Skip/Cancel/Replace with an unrelated sibling junction and rejection of selected links; multi-window Exit waits for canceled-job cleanup. Hidden synthetic windows and image fixtures only; no physical media or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());result=1;}
 SetEvent(gate.release);BackgroundCopies::CancelAll();FileOperations::CancelPending();ShutdownStorageUi();BackgroundCopies::enabled=false;BrowserTransfers::Clear();while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}
#endif
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
// This suite intentionally uses the same real hidden frames and clipboard
// commands as the background-copy suite, but injects failures at the device.
int RunFloppyCopyRegressionsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\floppy-copy-regressions-report.txt","w");if(!report)return 2;
 class FaultFloppy:public FloppyWriteApi {
 public:FakeFloppy disk;bool locked=false,denyLock=false,writeProtect=false,writeFail=false,shortWrite=false,flushed=false,badRead=false,shortRead=false,readFail=false,swapOnFlush=false,gateRead=false;DWORD flushError=ERROR_INVALID_FUNCTION;unsigned writes=0,readbacks=0,flushes=0;
 HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr),release=CreateEvent(nullptr,TRUE,FALSE,nullptr);
 explicit FaultFloppy(const std::vector<BYTE>& bytes):disk(bytes){}~FaultFloppy(){SetEvent(release);CloseHandle(entered);CloseHandle(release);}
 UINT DriveType(int n)override{return disk.DriveType(n);}HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{if(writeProtect&&(a&GENERIC_WRITE)){SetLastError(ERROR_WRITE_PROTECT);return INVALID_HANDLE_VALUE;}return disk.Open(p,a,s,f);}bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk.Geometry(h,g);}bool Verify(HANDLE h,DWORD& n)override{return disk.Verify(h,n);}void Close(HANDLE h)override{disk.Close(h);}
 bool Read(HANDLE h,ULONGLONG at,DWORD n,void* p,DWORD& got)override{
  bool checking=DirectFloppy::Current(25)&&flushed&&writes;if(checking){++readbacks;if(gateRead){SetEvent(entered);if(WaitForSingleObject(release,30000)!=WAIT_OBJECT_0){SetLastError(ERROR_TIMEOUT);return false;}}if(readFail){SetLastError(ERROR_READ_FAULT);return false;}}
  bool ok=disk.Read(h,at,n,p,got);if(ok&&checking){if(shortRead)--got;if(badRead&&got)static_cast<BYTE*>(p)[0]^=1;}return ok;
 }
 bool Lock(HANDLE)override{if(denyLock){SetLastError(ERROR_ACCESS_DENIED);return false;}locked=true;return true;}bool Dismount(HANDLE)override{return locked;}bool Unlock(HANDLE)override{locked=false;return true;}
 bool FormatTracks1440(HANDLE)override{throw HfsPlus::Error("File copy attempted to format floppy");}
 bool Write(HANDLE,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{Check(locked&&at<=disk.media.size()&&n<=disk.media.size()-at,"write outside locked simulated floppy");if(writeFail){SetLastError(ERROR_WRITE_FAULT);return false;}got=shortWrite?n-1:n;memcpy(disk.media.data()+size_t(at),p,got);++writes;return true;}
 bool Flush(HANDLE)override{++flushes;flushed=true;if(swapOnFlush)++disk.changeCount;if(flushError){SetLastError(flushError);return false;}return true;}
 void Reset(const std::vector<BYTE>& bytes){disk.media=bytes;++disk.changeCount;denyLock=writeProtect=writeFail=shortWrite=flushed=badRead=shortRead=readFail=swapOnFlush=gateRead=false;flushError=ERROR_INVALID_FUNCTION;writes=readbacks=flushes=0;ResetEvent(entered);ResetEvent(release);}
 };
 std::unique_ptr<FaultFloppy> device;bool attached=false;int result=0;auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_copyin_mode=app.m_copyout_mode=4;
 try{
  std::vector<BYTE> blank;std::string reason;Check(PrepareFloppyHfs("AuditFloppy",blank,reason),"format simulated audit disk");device.reset(new FaultFloppy(blank));readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,device.get()),"attach audit floppy");attached=true;
  HFSIFACE_set_copy_modes(4,4,0);app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create audit browser");auto& session=frame->GetBrowserSession();auto doc=session.Document();char physical[MAX_PATH];get_floppy_volume_file_name(25,physical);int index=doc->insert_volume(physical,1,0,0,0);Check(index>=0,"insert audit floppy");auto& mounted=doc->Mounts().ClassicAt(index);Check(doc->Mounts().OpenClassicForTest(index),"mount audit floppy");auto tree=session.Tree();doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},tree->initialize_volume(index,mounted.m_volume_name,2,1,0,0,0));auto root=doc->GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,index});
  CString source=host+"\\Payload";auto reset=[&]{session.AwaitNavigationForTest();doc->Mounts().CloseClassic(index);physical_floppy_suspend(25);device->Reset(blank);Check(doc->Mounts().OpenClassicForTest(index),"reopen reset audit floppy");bool notify=tree->SelectionNotificationsEnabled();tree->enable_selchange(0);tree->GetTreeCtrl().SelectItem(root);tree->delete_subdir_contents(root);tree->enable_selchange(notify);doc->open_sub_directory(index,2,mounted.m_volume_name);{CFile file;Check(file.Open(source,CFile::modeCreate|CFile::modeWrite),"create audit source");std::vector<BYTE> bytes(8192,0x42);file.Write(bytes.data(),UINT(bytes.size()));}storageUiTest={};};
  auto paste=[&](bool cut){FileOperations::Location from;from.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)source)));from.cut=cut;from.expectedIdentity=MacBrowser::DescribeWindowsFile(from.WindowsPath()).identity;BrowserTransfers::Publish({{from},true},false);frame->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Check(BackgroundCopies::Active()&&frame->IsWindowEnabled(),"floppy Paste blocked or lost background job");};
  auto done=[&]{Until([]{return !BackgroundCopies::Active();});session.AwaitNavigationForTest();Check(device->disk.aligned&&!device->locked&&VolumeCoord::Readable(VolumeCoord::DeviceKey(25))&&FileOperations::Pending().empty(),"operation leaked floppy access or used unaligned I/O");};
  auto present=[&]{return GetFileAttributesA(source)!=INVALID_FILE_ATTRIBUTES;};
  auto readFile=[&](const std::u16string& name){physical_floppy_suspend(25);auto volume=MacBrowser::OpenClassic(physical,"AuditFloppy:",false);auto id=Find(*volume,name);Check(id!=0,"floppy destination missing");std::vector<BYTE> bytes(size_t(volume->Stat(id).forks[0].size));volume->Read(id,0,0,bytes.data(),bytes.size());volume->Close();return bytes;};
  reset();paste(false);done();Check(!BackgroundCopies::LastResult().code&&present()&&readFile(u"Payload")==std::vector<BYTE>(8192,0x42),"ordinary original-style copy regressed");
  for(DWORD flush:{DWORD(0),DWORD(ERROR_INVALID_FUNCTION),DWORD(ERROR_NOT_SUPPORTED)}){reset();device->flushError=flush;paste(true);done();Check(!BackgroundCopies::LastResult().code&&!present()&&!BrowserTransfers::Any()&&device->readbacks>0&&readFile(u"Payload")==std::vector<BYTE>(8192,0x42),"verified converted Cut did not remove source and consume clipboard");}
  const char* cases[]={"lock refusal","failed write","short write","failed flush","readback mismatch","short readback","failed readback","media changed on flush","write protection"};
  for(unsigned fault=0;fault<ARRAYSIZE(cases);++fault){reset();switch(fault){case 0:device->denyLock=true;break;case 1:device->writeFail=true;break;case 2:device->shortWrite=true;break;case 3:device->flushError=ERROR_NOT_READY;break;case 4:device->badRead=true;break;case 5:device->shortRead=true;break;case 6:device->readFail=true;break;case 7:device->swapOnFlush=true;break;case 8:device->writeProtect=true;break;}
   paste(true);done();Check(BackgroundCopies::LastResult().code&&present()&&BrowserTransfers::Any()&&BrowserTransfers::Snapshot().items.front().cut,(std::string("failed Cut lost source/clipboard: ")+cases[fault]).c_str());if(fault==0||fault==8)Check(device->writes==0&&device->disk.media==blank,"denied lock changed media");fprintf(report,"PASS: %s retained source/Cut and released floppy access.\n",cases[fault]);
  }
  auto mutation=[&](const char* name,auto makeRequest){
   reset();paste(false);done();
   Check(!BackgroundCopies::LastResult().code&&device->readbacks>0,"ordinary floppy copy skipped readback");
   device->flushed=false;
   auto item=FileOperations::Location::Classic(physical,"AuditFloppy:Payload");
   item.BindMediaGeneration(device->disk.changeCount);
   auto before=device->disk.media;
   auto writes=device->writes;
   ++device->disk.changeCount;
   auto stale=FileOperations::Submit(makeRequest(item));
   stale.Job()->wait();
   auto rejected=stale.Job()->result();
   Check(rejected.error==ERROR_MEDIA_CHANGED&&device->writes==writes&&
         device->disk.media==before&&!device->locked,
         (std::string(name)+": stale request, error "+std::to_string(rejected.error)+" "+rejected.message).c_str());
   item.BindMediaGeneration(device->disk.changeCount);
   device->badRead=true;
   auto failed=FileOperations::Submit(makeRequest(item));
   failed.Job()->wait();
   auto failure=failed.Job()->result();
   // An HFS transaction can hit the fault at its own sync, then fail rollback
   // because the device error is already latched.
   Check(failure.error==ERROR_CRC&&failure.rollback>=Storage::Rollback::Unverified&&
         device->writes>writes&&!device->locked,
         (std::string(name)+": readback, error "+std::to_string(failure.error)+
          ", rollback "+std::to_string(int(failure.rollback))+" "+failure.message).c_str());
  };
  mutation("Floppy rename did not reject a stale generation or report failed readback",
      [](const auto& item){return FileOperations::RenameEntry{item,u"Changed"};});
  mutation("Floppy delete did not reject a stale generation or report failed readback",
      [](const auto& item){return FileOperations::DeleteEntries{{item}};});
  mutation("Floppy Finder edit did not reject a stale generation or report failed readback",
      [](const auto& item){return FileOperations::EditClassicProperties{item,FileOperations::FinderLabel{3}};});
  fprintf(report,"PASS: rename, delete and Finder edits reject stale media and report device readback failure.\n");
  reset();paste(false);done();
  {
   auto folder=FileOperations::Location::Classic(physical,"AuditFloppy:");
   folder.directory=true;folder.BindMediaGeneration(device->disk.changeCount);
   auto create=FileOperations::Submit(FileOperations::CreateFolder{folder,u"Collision"});
   create.Job()->wait();
   Check(!create.Job()->result().code,"classic adapter could not create a floppy directory");
   auto selected=FileOperations::Location::Classic(physical,"AuditFloppy:Payload");
   selected.BindMediaGeneration(device->disk.changeCount);
   const auto before=device->disk.media;
   auto collision=FileOperations::Submit(FileOperations::RenameEntry{selected,u"Collision"});
   collision.Job()->wait();
   Check(collision.Job()->result().code&&device->disk.media==before&&
         readFile(u"Payload")==std::vector<BYTE>(8192,0x42),
         "classic rename treated a folder collision as an implicit move");
   device->flushed=false;
   auto rename=FileOperations::Submit(FileOperations::RenameEntry{selected,u"Renamed"});
   rename.Job()->wait();
   Check(!rename.Job()->result().code&&readFile(u"Renamed")==std::vector<BYTE>(8192,0x42),
         "classic adapter floppy rename lost the original contents");
   selected.SetClassic(physical,"AuditFloppy:Renamed");
   selected.BindMediaGeneration(device->disk.changeCount);
   device->flushed=false;
   auto remove=FileOperations::Submit(FileOperations::DeleteEntries{{selected}});
   remove.Job()->wait();
   auto volume=MacBrowser::OpenClassic(physical,"AuditFloppy:",false);
   Check(!remove.Job()->result().code&&!Find(*volume,u"Renamed")&&Find(*volume,u"Collision"),
         "classic adapter floppy delete removed the wrong entry or failed to persist");
   volume->Close();
  }
  fprintf(report,"PASS: shared floppy rename rejects folder collisions and preserves data; delete persists.\n");
  testStage="floppy readback cancellation";reset();device->gateRead=true;paste(true);Until([&]{return WaitForSingleObject(device->entered,0)==WAIT_OBJECT_0;});Check(BackgroundCopies::Active()&&present()&&BrowserTransfers::Any()&&frame->IsWindowEnabled(),"source removed, job timed out or browser disabled before readback completed");SendMessage(BackgroundCopies::DialogWindow(),WM_COMMAND,IDCANCEL,0);SetEvent(device->release);done();Check(present()&&BrowserTransfers::Any()&&BackgroundCopies::LastResult().error==ERROR_CANCELLED,"cancel during verification removed source or lost Cancel result");
  // A mode question can outlive its selected disk. A replacement disk in
  // the same drive must never inherit that queued write request.
  testStage="floppy change during mode selection";reset();app.m_copyin_mode=6;paste(true);Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});++device->disk.changeCount;auto picker=BackgroundCopies::ModeWindow();SendDlgItemMessage(picker,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(picker,WM_COMMAND,IDOK,0);done();Check(BackgroundCopies::LastResult().code&&present()&&device->writes==0&&device->disk.media==blank,"queued copy wrote replacement media selected after mode prompt");app.m_copyin_mode=4;
  // Native folder Cut to a floppy, then a second Cut within that same floppy.
  testStage="native folder Cut to floppy";reset();auto native=Image(host+"\\native-source.dsk");auto branch=native->MakeDirectory(2,u"Tree");MacBrowser::Entry entry;entry.name=u"Item";entry.forks[0].size=123;entry.forks[1].size=17;entry.finder[0]='T';native->Put(branch,entry,[](unsigned fork,uint64_t at,void* data,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(data)[i]=BYTE(fork+at+i);},false);
  FileOperations::Location nativeFolder;nativeFolder.SetMounted(native,branch);nativeFolder.cut=true;nativeFolder.directory=true;device->badRead=true;BrowserTransfers::Publish({{nativeFolder},true},false);frame->SendMessage(WM_COMMAND,ID_EDIT_PASTE);done();Check(BackgroundCopies::LastResult().error==ERROR_CRC&&Find(*native,u"Tree")&&BrowserTransfers::Any(),"generic native Cut lost source/Cut or hardware error after bad floppy readback");reset();BrowserTransfers::Publish({{nativeFolder},true},false);frame->SendMessage(WM_COMMAND,ID_EDIT_PASTE);done();Check(!BackgroundCopies::LastResult().code&&!Find(*native,u"Tree")&&!BrowserTransfers::Any(),"native folder Cut to floppy failed or retained source");
  testStage="same-floppy folder Cut";auto destinationId=native->MakeDirectory(2,u"Destination");nativeFolder.SetId(destinationId);nativeFolder.cut=false;BrowserTransfers::Publish({{nativeFolder},true},false);frame->SendMessage(WM_COMMAND,ID_EDIT_PASTE);done();Until([&]{return Row(session,L"Destination")>=0&&Row(session,L"Tree")>=0;});Select(session,L"Tree");frame->SendMessage(WM_COMMAND,ID_EDIT_CUT);auto oldCatalogId=BrowserTransfers::Snapshot().items.front().expectedCatalogId;auto binding=session.List()->BrowserRowAt(Row(session,L"Destination"));Check(binding&&binding->treeItem,"same-floppy destination binding missing");tree->GetTreeCtrl().SelectItem(binding->treeItem);Check(session.AwaitNavigationForTest(),"select same-floppy destination");frame->SendMessage(WM_COMMAND,ID_EDIT_PASTE);done();Check(!BackgroundCopies::LastResult().code&&!BrowserTransfers::Any(),"same-floppy folder Cut did not finish");
  physical_floppy_suspend(25);{auto volume=MacBrowser::OpenClassic(physical,"AuditFloppy:",false);Check(!Find(*volume,u"Tree"),"same-floppy move did not persist source deletion");auto dest=Find(*volume,u"Destination");uint32_t treeId=0,itemId=0;for(auto& e:volume->List(dest))if(e.name==u"Tree")treeId=e.id;Check(treeId!=0&&volume->Stat(treeId).resolvedId==oldCatalogId,"same-floppy move lost its tree or changed catalog identity");for(auto& e:volume->List(treeId))if(e.name==u"Item")itemId=e.id;Check(itemId&&volume->Stat(itemId).forks[1].size==17,"same-floppy move lost resource fork");BYTE data[17]{};volume->Read(itemId,1,0,data,sizeof(data));for(unsigned i=0;i<sizeof(data);++i)Check(data[i]==BYTE(i+1),"same-floppy resource bytes differ");volume->Close();}
  // Same-volume native moves must use catalog rename, preserving identity
  // without decoding/copying file data at all.
  auto nativeFile=Put(*native,u"Identity");Gate noRead;noRead.fail=true;SetEvent(noRead.release);auto guarded=std::make_shared<Slow>(native,noRead);FileOperations::Request renameMove;FileOperations::Location nativeFrom,nativeTo;nativeFrom.SetMounted(guarded,nativeFile);nativeFrom.cut=true;nativeTo.SetMounted(native,destinationId);renameMove.Copy().sources={nativeFrom};renameMove.Copy().destination=nativeTo;Check(BackgroundCopies::Start({renameMove},frame,true),"start native in-place move");done();Check(!BackgroundCopies::LastResult().code&&native->Stat(nativeFile).parent==destinationId&&WaitForSingleObject(noRead.entered,0)==WAIT_TIMEOUT,"native same-volume move copied data or changed catalog identity");
  // Skip a duplicate, then cancel the following per-file mode question. No
  // invisible completed progress dialog may be left behind.
  testStage="mode Skip followed by cancellation";reset();paste(false);done();CString secondPath=host+"\\Second";{CFile file;Check(file.Open(secondPath,CFile::modeCreate|CFile::modeWrite),"create second planning source");file.Write("SECOND",6);}
  auto planned=[&](const CString& path){FileOperations::Request r;r = FileOperations::LegacyConversion{};r.Legacy().askMode=true;r.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)host,(LPCSTR)path);r.Legacy().destination=FileOperations::Location::FromLegacy(physical,"AuditFloppy:");r.Legacy().options.input=r.Legacy().options.output='r';r.Legacy().options.appledouble=false;auto from=FileOperations::Location::Windows(VolumeCoord::Wide(std::string((LPCSTR)path)));r.Legacy().source=std::move(from);r.Legacy().destination=session.MacBrowser().TargetFolder();r.Legacy().target=FileOperations::ConversionTarget::Folder;return r;};
  unsigned before=BackgroundCopies::DialogCount();bool plannedComplete=true;auto originalMedia=device->disk.media;Check(BackgroundCopies::Start({planned(source),planned(secondPath)},frame,false,[&](bool ok){plannedComplete=ok;}),"start per-file mode batch");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});picker=BackgroundCopies::ModeWindow();SendDlgItemMessage(picker,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(picker,WM_COMMAND,IDOK,0);Until([]{auto jobs=FileOperations::Pending();return !jobs.empty()&&!jobs.front().Job()->conflict().empty();});FileOperations::Pending().front().Job()->resolve(Storage::ConflictChoice::Skip);Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});SendMessage(BackgroundCopies::ModeWindow(),WM_CLOSE,0,0);done();Check(!plannedComplete&&present()&&GetFileAttributesA(secondPath)!=INVALID_FILE_ATTRIBUTES&&device->disk.media==originalMedia&&BackgroundCopies::DialogCount()==before,"mode cancel after Skip leaked progress or changed media/source");
  // Export a physical floppy Cut, then finalize deletion through a separate
  // verified source writer after the host destination has completed.
  testStage="physical Cut export";reset();paste(false);done();FileOperations::Location from,to;from.SetClassic(physical,"AuditFloppy:Payload");from.cut=true;to.SetWindows(VolumeCoord::Wide(std::string((LPCSTR)host)));
  Check(DeleteFileA(source),"remove original host file before export");Host(session,host);Check(session.MacBrowser().TransferSelection({from},to),"start physical-source Cut export");done();Check(!BackgroundCopies::LastResult().code&&present(),"physical-source Cut export failed");physical_floppy_suspend(25);{auto volume=MacBrowser::OpenClassic(physical,"AuditFloppy:",false);Check(!Find(*volume,u"Payload"),"physical-source Cut deletion was not finalized");volume->Close();}
  testStage="last browser close during mode selection";reset();app.m_copyin_mode=6;paste(true);Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});frame->SendMessage(WM_CLOSE);Until([&]{return app.m_browsers.empty();});Check(!BackgroundCopies::Active()&&present()&&device->writes==0,"closing last browser during mode selection wrote media or lost source");
  fprintf(report,"PASS: real hidden browser clipboard Paste, original fast copy, verified Cut with supported/unsupported flush, device readback before source deletion, cancellation during verification, queued-media replacement rejection, generic native Cut failure retention, native and same-floppy folder moves with resource forks, mode-cancel-after-Skip cleanup, last-window close during mode selection, physical-source export/delete finalization, source/Cut retention across nine injected device faults, unlock/lease cleanup. Simulated media only; no hardware or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s; last result: %s\n",e.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 if(device)SetEvent(device->release);BackgroundCopies::CancelAll();FileOperations::CancelPending();ShutdownStorageUi();BackgroundCopies::enabled=false;BrowserTransfers::Clear();while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);if(attached){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}device.reset();fclose(report);return result;
}

int RunIndependentStorageTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\independent-storage-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 BackgroundCopies::enabled=true;Gate stalled;std::vector<FileOperations::Operation> jobs;
 try{
  auto summary=TaskbarProgress::Combine({{false,1,4},{false,3,4}});Check(summary.active&&!summary.waiting&&summary.done*2==summary.total,"parallel taskbar progress must combine jobs");summary=TaskbarProgress::Combine({{false,3,4},{true,0,0}});Check(summary.active&&summary.waiting,"queued/scanning job must keep taskbar progress active");summary=TaskbarProgress::Combine({{false,3,4}});Check(summary.active&&summary.done*4==summary.total*3,"finishing one job must not clear another's taskbar progress");Check(!TaskbarProgress::Combine({}).active,"last taskbar job must clear progress");
  auto classic=[&](const char* leaf){CString path=host+"\\"+leaf;{CFile f;Check(f.Open(path,CFile::modeCreate|CFile::modeWrite),"create classic concurrency fixture");f.SetLength(8*1024*1024);}char label[]="SameLabel";Check(hfs_format(const_cast<char*>((LPCSTR)path),0,label)==0,"format classic concurrency fixture");return path;};
  auto aPath=classic("a.hfv"),bPath=classic("b.hfv"),cPath=classic("c.hfv");
  {auto a=MacBrowser::OpenClassic((LPCSTR)aPath,"SameLabel:",true);Put(*a,u"Input");a->Close();auto b=MacBrowser::OpenClassic((LPCSTR)bPath,"SameLabel:",true);Put(*b,u"Embedded");b->Close();}
  // Build a real converter fixture; concurrent raw and MacBinary imports must
  // retain distinct settings, callbacks, replacement paths and mount tables.
  CString binary=host+"\\fixture.bin";Storage::Job fixture;auto outcome=TransferFixture::Work((LPCSTR)bPath,"SameLabel:Embedded",(LPCSTR)host,(LPCSTR)binary,0,0,'m')(fixture);Check(!outcome.code,"build MacBinary concurrency fixture");
  CString input=host+"\\Input";{CFile f;Check(f.Open(input,CFile::modeCreate|CFile::modeWrite),"create raw import fixture");f.Write("RAW CONTENT",11);}
  auto request=[&](const CString& path,const CString& source,char mode){FileOperations::Request r;r = FileOperations::LegacyConversion{};r.background=true;r.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)host,(LPCSTR)source);r.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)path,"SameLabel:");r.Legacy().options.input=mode;r.Legacy().options.output='r';r.Legacy().options.appledouble=false;return r;};
  jobs.push_back(FileOperations::Submit(request(aPath,input,'r')));Until([&]{return !jobs.front().Job()->conflict().empty();});
  // More conflicting requests than worker slots must not starve a later
  // independent volume. Cancel them while queued, without touching that media.
  for(int n=0;n<7;++n)jobs.push_back(FileOperations::Submit(request(aPath,input,'r')));
  jobs.push_back(FileOperations::Submit(request(cPath,binary,'m')));auto independent=jobs.back().Job();Until([&]{return independent->done();});Check(!independent->result().code&&!jobs.front().Job()->done()&&jobs.front().Job()->conflict().find("Input")!=std::string::npos,"unrelated converter blocked or overwrote another job's callback");
  {auto c=MacBrowser::OpenClassic((LPCSTR)cPath,"SameLabel:",false);auto id=Find(*c,u"Embedded");Check(id&&c->Stat(id).forks[1].size==31&&!Find(*c,u"Input"),"concurrent converter settings/mounts leaked");BYTE bytes[31]{};c->Read(id,1,0,bytes,sizeof(bytes));for(unsigned n=0;n<sizeof(bytes);++n)Check(bytes[n]==BYTE(n+1),"concurrent converter corrupted resource fork");c->Close();}
  for(size_t n=1;n+1<jobs.size();++n)jobs[n].Job()->cancel();for(size_t n=1;n+1<jobs.size();++n)Until([&]{return jobs[n].Job()->done();});
  Check(!jobs.front().Job()->done()&&!VolumeCoord::Readable(VolumeCoord::ImageKey((LPCSTR)aPath)),"canceling queued copies released active writer");
  // An ordinary classic browser must read a different classic image while
  // the first converter still waits inside its overwrite callback.
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create concurrency browser");auto& session=frame->GetBrowserSession();auto doc=session.Document();int index=doc->insert_volume((LPCSTR)bPath,0,0,0,0);Check(index>=0&&doc->Mounts().OpenClassicForTest(index),"mount independent classic browser");auto& mounted=doc->Mounts().ClassicAt(index);doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},session.Tree()->initialize_volume(index,mounted.m_volume_name,2,0,0,0,0));doc->open_sub_directory(index,2,mounted.m_volume_name);Check(Row(session,L"Embedded")>=0&&!jobs.front().Job()->done(),"classic browsing waited on unrelated converter");
  jobs.front().Job()->resolve(Storage::ConflictChoice::Replace);Until([&]{return jobs.front().Job()->done();});Check(!jobs.front().Job()->result().code,"raw converter failed after independent work");{auto a=MacBrowser::OpenClassic((LPCSTR)aPath,"SameLabel:",false);auto id=Find(*a,u"Input");char bytes[11]{};Check(id&&a->Stat(id).forks[0].size==11,"raw import inherited MacBinary mode");a->Read(id,0,0,bytes,11);Check(!memcmp(bytes,"RAW CONTENT",11),"raw import data changed");a->Close();}
  fprintf(report,"PASS: simultaneous real classic raw/MacBinary converters, same-label mount isolation, per-job callbacks/settings/resource forks, seven queued conflicts without worker starvation, independent classic browser navigation, queued cancellation and writer exclusion.\n");

  Gate mutationGate;auto undo=Storage::copyWorker().submit([&](Storage::Job&){LegacyEngine::Scope engine;auto v=hfs_mount(const_cast<char*>((LPCSTR)aPath),0,O_RDWR);Check(v&&b_begin_mutation(v)==0,"begin isolated undo");SetEvent(mutationGate.entered);bool released=WaitForSingleObject(mutationGate.release,15000)==WAIT_OBJECT_0;int rolled=b_end_mutation(v,-1);int closed=hfs_umount(v);Check(released&&rolled<0&&closed==0,"isolated undo cleanup");return Storage::Result{};});
  struct ReleaseUndo{Gate& gate;std::shared_ptr<Storage::Job> job;~ReleaseUndo(){SetEvent(gate.release);job->wait();}}releaseUndo{mutationGate,undo};Until([&]{return WaitForSingleObject(mutationGate.entered,0)==WAIT_OBJECT_0;});
  auto mutate=Storage::copyWorker().submit([&](Storage::Job&){LegacyEngine::Scope engine;auto v=hfs_mount(const_cast<char*>((LPCSTR)bPath),0,O_RDWR);Check(v!=nullptr,"mount independent undo volume");char name[]=":Concurrent";int changed=hfs_mkdir(v,name);int closed=hfs_umount(v);Check(changed==0&&closed==0,"unrelated mutation inherited another volume's undo state");return Storage::Result{};});Until([&]{return mutate->done();});Check(!mutate->result().code&&!undo->done(),"independent undo operations serialized or failed");SetEvent(mutationGate.release);Until([&]{return undo->done();});Check(!undo->result().code,"first undo lost its state");
  fprintf(report,"PASS: classic mutation/rollback contexts run simultaneously on different volumes without sharing active undo state.\n");
  // Export overwrite questions must not leave the process cwd changed.
  CString exportDir=host+"\\exports";Check(CreateDirectoryA(exportDir,nullptr)!=FALSE,"create concurrent export directory");{CFile file;Check(file.Open(exportDir+"\\Embedded",CFile::modeCreate|CFile::modeWrite),"create export conflict");file.Write("KEEP",4);}
  char cwdBefore[MAX_PATH]{},cwdDuring[MAX_PATH]{};Check(GetCurrentDirectoryA(MAX_PATH,cwdBefore)>0,"capture cwd");FileOperations::Request exportRequest;exportRequest = FileOperations::LegacyConversion{};exportRequest.background=true;exportRequest.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)bPath,"SameLabel:Embedded");exportRequest.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)host,(LPCSTR)exportDir);exportRequest.Legacy().options.output='r';exportRequest.Legacy().options.appledouble=false;auto exportJob=FileOperations::Submit(exportRequest);jobs.push_back(exportJob);Until([&]{return !exportJob.Job()->conflict().empty();});Check(GetCurrentDirectoryA(MAX_PATH,cwdDuring)>0&&!strcmp(cwdBefore,cwdDuring),"converter changed the process working directory while awaiting overwrite");exportJob.Job()->resolve(Storage::ConflictChoice::Cancel);Until([&]{return exportJob.Job()->done();});Check(exportJob.Job()->result().error==ERROR_CANCELLED,"cancel export conflict");
  // Interleave two encoders with the first paused after partial data and CRC
  // state have been accumulated. File/buffer/CRC/RLE state must stay private.
  Gate encoderGate;std::vector<BYTE> payloadA(1025,0x90),payloadB(2049,0x41);payloadA[500]=0;payloadB[1000]=0x90;CString hqxA=host+"\\one.hqx",hqxB=host+"\\two.hqx";
  auto encode=[&](const CString& path,std::vector<BYTE>& payload,Gate* gate){return Storage::copyWorker().submit([path,&payload,gate](Storage::Job&){int fd=_open(path,_O_CREAT|_O_EXCL|_O_WRONLY|_O_BINARY,_S_IREAD|_S_IWRITE);Check(fd>=0&&bh_start(fd)==0,"open isolated BinHex encoder");Check(bh_insert(payload.data(),500)==0,"encode isolated first chunk");if(gate){SetEvent(gate->entered);Check(WaitForSingleObject(gate->release,15000)==WAIT_OBJECT_0,"BinHex encoder gate");}Check(bh_insert(payload.data()+500,int(payload.size()-500))==0&&bh_insertcrc()==0&&bh_end()==0,"finish isolated BinHex encoder");_close(fd);return Storage::Result{};});};
  auto encoderOne=encode(hqxA,payloadA,&encoderGate);struct ReleaseEncoder{Gate& gate;std::shared_ptr<Storage::Job> job;~ReleaseEncoder(){SetEvent(gate.release);job->wait();}}releaseEncoder{encoderGate,encoderOne};Until([&]{return WaitForSingleObject(encoderGate.entered,0)==WAIT_OBJECT_0;});auto encoderTwo=encode(hqxB,payloadB,nullptr);Until([&]{return encoderTwo->done();});Check(!encoderTwo->result().code&&!encoderOne->done(),"BinHex encoder state was serialized or corrupted");SetEvent(encoderGate.release);Until([&]{return encoderOne->done();});Check(!encoderOne->result().code,"first BinHex encoder lost state");
  auto decode=[&](const CString& path,const std::vector<BYTE>& expected){int fd=_open(path,_O_RDONLY|_O_BINARY);Check(fd>=0&&bh_open(fd)==0,"open isolated BinHex output");std::vector<BYTE> decoded(expected.size());Check(bh_read(decoded.data(),int(decoded.size()))==decoded.size()&&bh_readcrc()==0&&bh_close()==0,"isolated BinHex CRC failed");_close(fd);Check(decoded==expected,"interleaved BinHex bytes changed");};decode(hqxA,payloadA);decode(hqxB,payloadB);
  // A captured classic adapter retains its mount context across worker migration.
  {auto first=MacBrowser::OpenClassic((LPCSTR)bPath,"SameLabel:",true);auto alias=MacBrowser::OpenClassic((LPCSTR)bPath,"SameLabel:",true);Check(Find(*first,u"Embedded")&&Find(*alias,u"Embedded"),"same-volume classic adapters did not share their mount context");alias->Close();Check(Find(*first,u"Embedded")!=0,"closing an alias discarded the surviving classic mount");first->Close();}
  auto migrated=MacBrowser::OpenClassic((LPCSTR)bPath,"SameLabel:",false);auto migrateRead=Storage::copyWorker().submit([migrated](Storage::Job&){Check(Find(*migrated,u"Embedded")!=0,"classic adapter lost its context on another worker");return Storage::Result{};});Until([&]{return migrateRead->done();});Check(!migrateRead->result().code,"classic adapter migration failed");migrated->Close();
  fprintf(report,"PASS: process cwd remains stable during export overwrite; interleaved BinHex encoding preserves files, CRC and RLE state; classic adapter context survives worker migration.\n");
  // Separate real browser copy batches: a gated slow native read must not
  // serialize an unrelated classic copy, and canceling one must not cancel the other.
  auto source=Image(host+"\\source.dsk"),target=Image(host+"\\target.dsk");auto id=Put(*source,u"Slow");auto slow=std::make_shared<Slow>(source,stalled);FileOperations::Request r;FileOperations::Location from,to;from.SetMounted(slow,id);to.SetMounted(target);r.Copy().sources={from};r.Copy().destination=to;
  Check(BackgroundCopies::Start({r},frame,true),"start slow independent batch");Until([&]{return WaitForSingleObject(stalled.entered,0)==WAIT_OBJECT_0;});
  auto reservedImage=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::ImageKey((LPCSTR)bPath),VolumeCoord::Mode::Write}});startupImage=bPath;auto extraBrowser=app.CreateBrowser(FALSE);startupImage="";Check(extraBrowser&&app.m_browsers.size()==2&&BackgroundCopies::Active(),"New browser still blocked by an unrelated copy");auto& extraSession=extraBrowser->GetBrowserSession();auto extraDoc=extraSession.Document();Check(extraDoc->Mounts().ClassicCount()==1&&!extraDoc->Mounts().ClassicAt(0).m_free&&!extraDoc->Mounts().ClassicAt(0).m_opened&&extraSession.MountRoot({BrowserMountInventory::Kind::ClassicVolume,0}),"new browser opened or discarded a reserved startup image");Host(extraSession,host);reservedImage.reset();extraDoc->open_sub_directory(0,2,"");Check(Row(extraSession,L"Embedded")>=0&&extraDoc->Mounts().ClassicAt(0).m_opened&&extraBrowser->IsWindowEnabled(),"new browser could not load an unrelated image after its reservation ended");
  auto other=FileOperations::Submit(request(cPath,input,'r'));jobs.push_back(other);Until([&]{return other.Job()->done();});Check(!other.Job()->result().code&&BackgroundCopies::Active(),"slow batch serialized unrelated classic copy");doc->open_sub_directory(index,2,mounted.m_volume_name);Check(Row(session,L"Embedded")>=0,"slow native copy blocked classic browser");
  BackgroundCopies::CancelAll();SetEvent(stalled.release);Until([]{return !BackgroundCopies::Active();});Check(BackgroundCopies::LastResult().error==ERROR_CANCELLED&&!Find(*target,u"Slow")&&Find(*source,u"Slow"),"independent cancellation lost source or published partial target");
  fprintf(report,"PASS: slow native read allows unrelated classic copy and browser navigation; cancellation retains source, removes partial target and releases reservations.\n");
#ifdef HFV_FLOPPY_TESTING
  // Two physical-device adapters must have independent locks, caches,
  // descriptors and thread-local DirectFloppy writers. No hardware is opened.
  CString floppyPath=host+"\\floppy.dsk";{CFile f;Check(f.Open(floppyPath,CFile::modeCreate|CFile::modeWrite),"create concurrent floppy fixture");f.SetLength(1474560);}char label[]="Floppy";Check(hfs_format(const_cast<char*>((LPCSTR)floppyPath),0,label)==0,"format concurrent floppy fixture");std::vector<BYTE> bytes(1474560);{CFile f;Check(f.Open(floppyPath,CFile::modeRead)&&f.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"load concurrent floppy fixture");}
  struct GatedDisk:TestFloppy{Gate& gate;GatedDisk(const std::vector<BYTE>& b,Gate& g):TestFloppy(b),gate(g){}bool Write(HANDLE h,ULONGLONG at,DWORD n,const void* p,DWORD& got)override{SetEvent(gate.entered);if(WaitForSingleObject(gate.release,30000)!=WAIT_OBJECT_0){SetLastError(ERROR_TIMEOUT);return false;}return TestFloppy::Write(h,at,n,p,got);}};
  Gate deviceGate;GatedDisk diskA(bytes,deviceGate);TestFloppy diskB(bytes);readonly_cd_backend_init();Check(readonly_floppy_attach_api(24,&diskA)&&readonly_floppy_attach_api(25,&diskB),"attach independent floppy devices");
  struct Devices{Gate& gate;~Devices(){SetEvent(gate.release);FileOperations::CancelPending();for(auto& op:FileOperations::Pending())op.Job()->wait();readonly_floppy_attach_api(24,nullptr);readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}}devices{deviceGate};
  char physicalA[MAX_PATH],physicalB[MAX_PATH];get_floppy_volume_file_name(24,physicalA);get_floppy_volume_file_name(25,physicalB);
  auto physicalRequest=[&](const char* path){auto r=request(path,input,'r');r.SetLegacyTargetPath("Floppy:");r.Legacy().options.verifyFloppy=true;return r;};
  auto physicalOne=FileOperations::Submit(physicalRequest(physicalA));jobs.push_back(physicalOne);Until([&]{return WaitForSingleObject(deviceGate.entered,0)==WAIT_OBJECT_0;});auto physicalTwo=FileOperations::Submit(physicalRequest(physicalB));jobs.push_back(physicalTwo);Until([&]{return physicalTwo.Job()->done();});Check(!physicalTwo.Job()->result().code&&!physicalOne.Job()->done()&&diskA.locked&&!diskB.locked,"one floppy serialized or unlocked a different drive");SetEvent(deviceGate.release);Until([&]{return physicalOne.Job()->done();});Check(!physicalOne.Job()->result().code&&!diskA.locked&&diskA.writes&&diskB.writes&&diskA.disk.aligned&&diskB.disk.aligned,"parallel physical writes failed or leaked locks");
  for(const char* path:{physicalA,physicalB}){auto disk=MacBrowser::OpenClassic(path,"Floppy:",false);auto id=Find(*disk,u"Input");char data[11]{};Check(id!=0,"parallel floppy import missing");disk->Read(id,0,0,data,11);Check(!memcmp(data,"RAW CONTENT",11),"parallel floppy data mixed");disk->Close();}
  fprintf(report,"PASS: independent simulated physical floppies write/verify concurrently while one device stalls; descriptor, cache, alignment and lock cleanup checks. No physical drives opened.\n");
#endif

 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());result=1;}
 startupImage="";SetEvent(stalled.release);BackgroundCopies::CancelAll();for(auto& job:jobs)job.Job()->cancel();for(auto& job:jobs)job.Job()->wait();Until([]{return !BackgroundCopies::Active();});ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunBackgroundWindowCloseTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\background-window-close-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;app.m_copyin_mode=app.m_copyout_mode=0;
 Gate gate,deviceGate;
#ifdef HFV_FLOPPY_TESTING
 struct ReadGateDisk:TestFloppy{Gate& gate;bool armed=false;ReadGateDisk(const std::vector<BYTE>& bytes,Gate& g):TestFloppy(bytes),gate(g){}bool Read(HANDLE h,ULONGLONG at,DWORD count,void* p,DWORD& got)override{if(armed){SetEvent(gate.entered);if(WaitForSingleObject(gate.release,15000)!=WAIT_OBJECT_0){SetLastError(ERROR_TIMEOUT);got=0;return false;}}return TestFloppy::Read(h,at,count,p,got);}};
 std::unique_ptr<ReadGateDisk> physical;bool attached=false;
#endif
 try{
  struct Dialog: CDialog{explicit Dialog(UINT id):CDialog(id,CWnd::GetDesktopWindow()){}void OnCancel()override{DestroyWindow();}LRESULT WindowProc(UINT message,WPARAM w,LPARAM l)override{if(message==WM_CLOSE){OnCancel();return 0;}return CDialog::WindowProc(message,w,l);}};
  for(UINT resource:{UINT(IDD_ABOUTBOX),UINT(IDD_OPTIONS_PAGE_1)}){Dialog dialog(resource);Check(dialog.Create(resource,CWnd::GetDesktopWindow())!=FALSE,"create ordinary dialog");HWND window=dialog.GetSafeHwnd();DialogClose::test={};DialogClose::test.answer=IDNO;SendMessage(window,WM_SYSCOMMAND,SC_CLOSE,0);Check(!IsWindow(window)&&DialogClose::test.prompts==0,"ordinary dialog still asks before closing");}
  struct Sheet:CPropertySheet{Sheet():CPropertySheet("Hidden Options",CWnd::GetDesktopWindow()){}LRESULT WindowProc(UINT m,WPARAM w,LPARAM l)override{if(m==WM_CLOSE){DestroyWindow();return 0;}return CPropertySheet::WindowProc(m,w,l);}}sheet;CPropertyPage page(IDD_OPTIONS_PAGE_1);sheet.AddPage(&page);Check(sheet.Create(CWnd::GetDesktopWindow(),WS_POPUP|WS_CAPTION|WS_SYSMENU)!=FALSE,"create ordinary Options");HWND sheetWindow=sheet.GetSafeHwnd();DialogClose::test={};DialogClose::test.answer=IDNO;SendMessage(sheetWindow,WM_SYSCOMMAND,SC_CLOSE,0);Check(!IsWindow(sheetWindow)&&DialogClose::test.prompts==0,"Options still asks before closing");DialogClose::test={};
  fprintf(report,"PASS: About, General and Options property-sheet X closes normally without confirmation.\n");
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);Check(a&&b,"create closing-copy browsers");
  auto idle=app.CreateBrowser(FALSE);Check(idle!=nullptr,"create idle close browser");HWND idleWindow=idle->GetSafeHwnd();DialogClose::test={};DialogClose::test.answer=IDNO;idle->SendMessage(WM_SYSCOMMAND,SC_CLOSE);Check(IsWindow(idleWindow)&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::Browser,"idle browser X refusal ignored");DialogClose::test.answer=IDYES;idle->SendMessage(WM_SYSCOMMAND,SC_CLOSE);Check(!IsWindow(idleWindow)&&app.m_browsers.size()==2&&DialogClose::test.prompts==2,"idle browser X confirmation ignored");
  auto source=Image(host+"\\source.dsk"),target=Image(host+"\\target.dsk");auto slow=std::make_shared<Slow>(source,gate);
  auto noQuit=[&]{MSG message;Check(!PeekMessage(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"closing a non-last browser posted WM_QUIT");};
  auto request=[&](uint32_t id,bool cut){FileOperations::Request r;FileOperations::Location from,to;from.SetMounted(slow,id);from.cut=cut;to.SetMounted(target,2);r.Copy().sources={from};r.Copy().destination=to;return r;};
  auto finished=[&]{Until([]{return !BackgroundCopies::Active();});Check(FileOperations::Pending().empty()&&VolumeCoord::Readable(source->ResourceKey())&&VolumeCoord::Readable(target->ResourceKey()),"closed-window copy leaked reservations");};
  auto verify=[&](MacBrowser::Volume& volume,const std::u16string& name){auto id=Find(volume,name);Check(id!=0,"closed-window output missing");auto info=volume.Stat(id);Check(info.forks[0].size==8192&&info.forks[1].size==31&&info.finder[0]=='T',"closed-window output lost forks or metadata");std::vector<BYTE> data(8192);volume.Read(id,0,0,data.data(),data.size());for(size_t n=0;n<data.size();++n)Check(data[n]==BYTE(n),"closed-window data differs");BYTE resource[31]{};volume.Read(id,1,0,resource,sizeof(resource));for(unsigned n=0;n<31;++n)Check(resource[n]==BYTE(n+1),"closed-window resource differs");};
  // Real clipboard Cut/Paste survives two successive owner closures and keeps
  // command routing, progress, taskbar contributions and completion independent.
  auto payload=Put(*source,u"Payload");auto first=request(payload,true);auto& session=a->GetBrowserSession();int destination=session.MacBrowser().AttachTestVolume(target,"close-target");Check(session.MacBrowser().ShowTestFolder(destination,2),"show closing-copy target");BrowserTransfers::Publish({first.Copy().sources,true},false);gate.Reset();storageUiTest={};a->SendMessage(WM_COMMAND,ID_EDIT_PASTE);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto job=FileOperations::Pending().front().Job();HWND progress=BackgroundCopies::DialogWindow(),old=a->GetSafeHwnd(),survivor=b->GetSafeHwnd();Check(BackgroundCopies::TaskbarJobs(old)==1,"initial taskbar contribution missing");app.ActivateBrowser(a);DialogClose::test={};DialogClose::test.answer=IDNO;a->SendMessage(WM_SYSCOMMAND,SC_CLOSE);Check(IsWindow(old)&&!job->cancelled()&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::BackgroundBrowser,"non-last browser X refusal changed copy");DialogClose::test.answer=IDYES;a->SendMessage(WM_SYSCOMMAND,SC_CLOSE);
  Check(!IsWindow(old)&&IsWindow(survivor)&&app.m_pMainWnd==b&&app.ActiveBrowser()==b&&BackgroundCopies::Active()&&!job->cancelled(),"first browser did not close independently");noQuit();Check(IsWindow(progress)&&BackgroundCopies::DialogWindow()==progress&&BackgroundCopies::ProgressOwners(survivor)==1&&BackgroundCopies::TaskbarJobs(old)==0&&BackgroundCopies::TaskbarJobs(survivor)==1,"progress/taskbar did not migrate to surviving browser");
  Host(b->GetBrowserSession(),host);b->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check((b->GetBrowserSession().List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&b->IsWindowEnabled(),"surviving browser command routing failed");auto current=app.CreateBrowser(FALSE);Check(current!=nullptr,"new browser while original closes");HWND second=b->GetSafeHwnd();b->SendMessage(WM_CLOSE);Check(!IsWindow(second)&&BackgroundCopies::ProgressOwners(current->GetSafeHwnd())==1&&BackgroundCopies::TaskbarJobs(second)==0&&BackgroundCopies::TaskbarJobs(current->GetSafeHwnd())==1&&!job->cancelled(),"second closure canceled the job or lost progress");noQuit();SetEvent(gate.release);finished();Check(!job->result().code&&!Find(*source,u"Payload")&&!BrowserTransfers::Any(),"Cut did not finish after originating windows closed");verify(*target,u"Payload");
  fprintf(report,"PASS: first/main and subsequent windows close during a stalled clipboard Cut; progress/taskbar ownership migrates, surviving commands work, no early WM_QUIT, both forks/Finder metadata and Cut completion survive.\n");
  // Cancel remains available after the submitting window no longer exists.
  auto canceled=Put(*source,u"Canceled");gate.Reset();bool complete=true;Check(BackgroundCopies::Start({request(canceled,true)},current,true,[&](bool ok){complete=ok;}),"start closing cancellation");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto next=app.CreateBrowser(FALSE);old=current->GetSafeHwnd();progress=BackgroundCopies::DialogWindow();job=FileOperations::Pending().front().Job();current->SendMessage(WM_CLOSE);Check(!IsWindow(old)&&BackgroundCopies::Active()&&!job->cancelled(),"close prematurely canceled copy");noQuit();DialogClose::test={};DialogClose::test.answer=IDNO;SendMessage(progress,WM_SYSCOMMAND,SC_CLOSE,0);Check(!job->cancelled()&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::Copy,"copy X refusal still canceled its job");DialogClose::test.answer=IDYES;SendMessage(progress,WM_SYSCOMMAND,SC_CLOSE,0);Check(job->cancelled()&&IsWindow(progress)&&IsWindow(next->GetSafeHwnd())&&DialogClose::test.prompts==2,"confirmed copy X did not retain progress for cleanup");SetEvent(gate.release);finished();Check(!complete&&job->result().error==ERROR_CANCELLED&&Find(*source,u"Canceled")&&!Find(*target,u"Canceled"),"cancel after close lost source or published incomplete output");if(IsWindow(progress)){DialogClose::test={};DialogClose::test.answer=IDNO;SendMessage(progress,WM_SYSCOMMAND,SC_CLOSE,0);Check(!IsWindow(progress)&&DialogClose::test.prompts==0,"completed progress still asks before closing");}current=next;
  // Overwrite questions and errors also belong to the copy, not its old frame.
  auto failure=Put(*source,u"Failure");auto original=Put(*target,u"Failure");gate.Reset();gate.fail=true;storageUiTest={};Check(BackgroundCopies::Start({request(failure,false)},current,true),"start closing overwrite/error");Until([]{auto pending=FileOperations::Pending();return !pending.empty()&&!pending.front().Job()->conflict().empty();});job=FileOperations::Pending().front().Job();next=app.CreateBrowser(FALSE);old=current->GetSafeHwnd();progress=BackgroundCopies::DialogWindow();current->SendMessage(WM_CLOSE);Check(!IsWindow(old)&&!job->cancelled()&&!job->conflict().empty()&&IsWindow(progress),"closing window dismissed overwrite question");noQuit();job->resolve(Storage::ConflictChoice::Replace);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});SetEvent(gate.release);finished();Check(job->result().code&&Find(*source,u"Failure")&&target->Stat(original).id==original&&IsWindow(progress)&&next->IsWindowEnabled(),"error after close lost source/old target or progress");verify(*target,u"Failure");char phase[64]{};GetDlgItemTextA(progress,IDC_CP_PHASE,phase,ARRAYSIZE(phase));Check(!strcmp(phase,"Copy failed"),"closed-owner failure was not presented in its progress window");SendMessage(progress,WM_COMMAND,IDCANCEL,0);current=next;gate.fail=false;
  fprintf(report,"PASS: cancellation after owner closure retains source and removes partial output; overwrite decisions and replacement errors remain usable after closure and preserve previous content.\n");
  // The modeless conversion question must survive closure, with captured paths.
  CString classicPath=host+"\\classic.dsk";{CFile file;Check(file.Open(classicPath,CFile::modeCreate|CFile::modeWrite),"create close-mode fixture");file.SetLength(4*1024*1024);}char label[]="Closing";Check(hfs_format(const_cast<char*>((LPCSTR)classicPath),0,label)==0,"format close-mode fixture");{auto disk=MacBrowser::OpenClassic((LPCSTR)classicPath,"Closing:",true);Put(*disk,u"Converted");disk->Close();}
  auto modeRequest=[&]{FileOperations::Request r;r = FileOperations::LegacyConversion{};r.Legacy().askMode=true;r.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)classicPath,"Closing:Converted");r.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)host,(LPCSTR)(host+"\\exported"));r.Legacy().options.appledouble=false;return r;};complete=false;Check(BackgroundCopies::Start({modeRequest()},current,true,[&](bool ok){complete=ok;}),"start close-mode question");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});HWND picker=BackgroundCopies::ModeWindow();DialogClose::test={};DialogClose::test.answer=IDNO;SendMessage(picker,WM_SYSCOMMAND,SC_CLOSE,0);Check(IsWindow(picker)&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::CopyMode,"copy-mode X refusal dismissed its question");next=app.CreateBrowser(FALSE);old=current->GetSafeHwnd();current->SendMessage(WM_CLOSE);Check(!IsWindow(old)&&IsWindow(picker)&&BackgroundCopies::ProgressOwners(next->GetSafeHwnd())==1,"closing owner dismissed conversion question");noQuit();SendDlgItemMessage(picker,IDC_COPY_MODE,CB_SETCURSEL,4,0);SendMessage(picker,WM_COMMAND,IDOK,0);finished();{CFile exported;Check(complete&&exported.Open(host+"\\exported",CFile::modeRead)&&exported.GetLength()==8192,"conversion after close lost captured destination");}current=next;
  complete=true;Check(BackgroundCopies::Start({modeRequest()},current,true,[&](bool ok){complete=ok;}),"start mode-X cancellation");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});picker=BackgroundCopies::ModeWindow();DialogClose::test={};DialogClose::test.answer=IDYES;SendMessage(picker,WM_SYSCOMMAND,SC_CLOSE,0);finished();Check(!complete&&!IsWindow(picker)&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::CopyMode,"confirmed mode X did not cancel cleanly before I/O");
  fprintf(report,"PASS: modeless copy-mode question survives browser closure and exports to its captured destination.\n");
#ifdef HFV_FLOPPY_TESTING
  // Hold a real adapter device mutex inside an injected read. Closing its
  // browser must destroy the frame without waiting for that mutex on the UI.
  CString floppyPath=host+"\\floppy.dsk";{CFile file;Check(file.Open(floppyPath,CFile::modeCreate|CFile::modeWrite),"create close-floppy fixture");file.SetLength(1474560);}char floppyLabel[]="CloseFloppy";Check(hfs_format(const_cast<char*>((LPCSTR)floppyPath),0,floppyLabel)==0,"format close-floppy fixture");{auto disk=MacBrowser::OpenClassic((LPCSTR)floppyPath,"CloseFloppy:",true);Put(*disk,u"DevicePayload");disk->Close();}std::vector<BYTE> bytes(1474560);{CFile file;Check(file.Open(floppyPath,CFile::modeRead)&&file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"load close-floppy fixture");}physical.reset(new ReadGateDisk(bytes,deviceGate));readonly_cd_backend_init();Check(readonly_floppy_attach_api(25,physical.get()),"attach close-floppy reader");attached=true;char path[MAX_PATH];get_floppy_volume_file_name(25,path);
  auto mount=[&](CMainFrame* frame){auto& s=frame->GetBrowserSession();int index=s.Document()->insert_volume(path,1,0,0,0);Check(index>=0&&s.Document()->Mounts().OpenClassicForTest(index),"open closing physical reader");auto& v=s.Document()->Mounts().ClassicAt(index);s.Document()->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},s.Tree()->initialize_volume(index,v.m_volume_name,2,1,0,0,0));};mount(current);
  auto physicalRequest=[&]{FileOperations::Request r;FileOperations::Location from,to;from.SetClassic(path,"CloseFloppy:DevicePayload");to.SetMounted(target,2);r.Copy().sources={from};r.Copy().destination=to;return r;};deviceGate.Reset();physical->armed=true;Check(BackgroundCopies::Start({physicalRequest()},current,true),"start physical read during close");Until([&]{return WaitForSingleObject(deviceGate.entered,0)==WAIT_OBJECT_0;});job=FileOperations::Pending().front().Job();next=app.CreateBrowser(FALSE);old=current->GetSafeHwnd();current->SendMessage(WM_CLOSE);Check(!IsWindow(old)&&BackgroundCopies::Active()&&!job->done()&&!job->cancelled()&&WaitForSingleObject(deviceGate.release,0)==WAIT_TIMEOUT&&BrowserLifetime::RetiredCount()==1,"physical reader close blocked on I/O or dropped live reader state");noQuit();SetEvent(deviceGate.release);finished();Check(!job->result().code&&BrowserLifetime::RetiredCount()==0&&flcds[25].refcount==0,"physical reader retirement did not drain after copy");verify(*target,u"DevicePayload");current=next;
  fprintf(report,"PASS: closing a physical-media browser returns before its gated device read; viewless reader resources drain on the UI thread after copies finish, without stale subscriptions or leaked references.\n");
  // Closing the last frame cancels running, queued and mode-pending work and
  // keeps that frame alive until the blocked device call can safely unwind.
  physical->armed=false;mount(current);deviceGate.Reset();physical->armed=true;Check(BackgroundCopies::Start({physicalRequest()},current,true),"start last-window physical read");Until([&]{return WaitForSingleObject(deviceGate.entered,0)==WAIT_OBJECT_0;});job=FileOperations::Pending().front().Job();auto mustRetain=Put(*source,u"MustRetain");Check(BackgroundCopies::Start({request(mustRetain,true)},current,true),"queue last-window Cut");auto pending=FileOperations::Pending();Check(pending.size()==2,"last-window queue missing");auto queued=pending.back().Job();Check(BackgroundCopies::Start({modeRequest()},current,true),"start last-window mode question");Until([]{return BackgroundCopies::ModeWindow()!=nullptr;});picker=BackgroundCopies::ModeWindow();old=current->GetSafeHwnd();DialogClose::test={};DialogClose::test.answer=IDNO;current->SendMessage(WM_SYSCOMMAND,SC_CLOSE);Check(IsWindow(old)&&!job->cancelled()&&!queued->cancelled()&&IsWindow(picker)&&DialogClose::test.prompts==1&&DialogClose::test.kind==DialogClose::Kind::LastBrowser,"last-browser X refusal changed pending copies");DialogClose::test.answer=IDYES;current->SendMessage(WM_SYSCOMMAND,SC_CLOSE);Check(DialogClose::test.prompts==2,"shutdown cleanup asked additional close confirmations");fprintf(report,"Last-close state: frames=%u window=%d active=%d running_cancel=%d queued_cancel=%d mode_window=%d busy=%d session=%d\n",unsigned(app.m_browsers.size()),IsWindow(old),BackgroundCopies::Active(),job->cancelled(),queued->cancelled(),IsWindow(picker),StorageUi::busy(),StorageUi::state().sessionOpen);fflush(report);Check(IsWindow(old)&&BackgroundCopies::Active()&&job->cancelled()&&queued->cancelled()&&!IsWindow(picker),"last window closed before canceling/cleaning every batch");noQuit();Until([&]{return queued->done();});SetEvent(deviceGate.release);Until([&]{return app.m_browsers.empty();});Check(!IsWindow(old)&&!BackgroundCopies::Active()&&FileOperations::Pending().empty()&&BrowserLifetime::RetiredCount()==0&&flcds[25].refcount==0&&Find(*source,u"MustRetain")&&!Find(*target,u"MustRetain"),"last-window cleanup leaked state or lost queued Cut source");verify(*target,u"DevicePayload");Check(app.ActiveBrowser()==nullptr&&app.m_pMainWnd==nullptr,"last-window MFC ownership survived");
  fprintf(report,"PASS: last-window close cancels running/queued/mode-pending work, waits for device I/O and reservation cleanup, retains source/previous output, releases physical readers and only then destroys the final frame. Simulated media only.\n");
#endif
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());result=1;}
 SetEvent(gate.release);SetEvent(deviceGate.release);BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});ShutdownStorageUi();DialogClose::test={};BackgroundCopies::enabled=false;BrowserTransfers::Clear();while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);BrowserLifetime::Collect();
#ifdef HFV_FLOPPY_TESTING
 if(attached){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}physical.reset();
#endif
 fclose(report);return result;
}

int RunBackgroundMutationsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\background-mutations-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;Gate gate;
 struct Delayed:MacBrowser::Volume{
  std::shared_ptr<MacBrowser::Volume> base;Gate& gate;std::atomic<int> mode{0};uint32_t selected=0;bool fail=false;std::atomic<unsigned> removed{0},renamed{0};
  Delayed(std::shared_ptr<MacBrowser::Volume> v,Gate& g):base(v),gate(g){}
  void Hold(int operation,uint32_t id){if(mode==operation&&(!selected||id==selected)){SetEvent(gate.entered);Check(WaitForSingleObject(gate.release,15000)==WAIT_OBJECT_0,"mutation gate not released");if(fail)throw HfsPlus::Error("Injected mutation failure");}}
  std::string ResourceKey()const override{return base->ResourceKey();}std::u16string Name()const override{return base->Name();}const char* Kind()const override{return base->Kind();}bool Writable()const override{return base->Writable();}bool CaseSensitive()const override{return base->CaseSensitive();}uint64_t FreeBytes()const override{return base->FreeBytes();}void Reload()override{base->Reload();}
  MacBrowser::Entry Stat(uint32_t id)override{Hold(1,id);return base->Stat(id);}std::vector<MacBrowser::Entry> List(uint32_t id)override{return base->List(id);}void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override{base->Read(id,fork,at,p,n);}
  uint32_t MakeDirectory(uint32_t id,const std::u16string& name)override{return base->MakeDirectory(id,name);}uint32_t Put(uint32_t id,const MacBrowser::Entry& e,MacBrowser::Reader reader,bool replace)override{return base->Put(id,e,reader,replace);}void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{Hold(2,id);base->Rename(id,parent,name);++renamed;}void ValidateRemoval(uint32_t id)override{Hold(3,id);base->ValidateRemoval(id);}void Remove(uint32_t id)override{Hold(4,id);base->Remove(id);++removed;}
 };
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create mutation browser");auto base=Image(host+"\\mutations.dsk"),other=Image(host+"\\other.dsk");auto delayed=std::make_shared<Delayed>(base,gate);auto id=Put(*base,u"Original");auto mount=frame->GetBrowserSession().MacBrowser().AttachTestVolume(delayed,"mutations");Check(frame->GetBrowserSession().MacBrowser().ShowTestFolder(mount,2),"show mutation source");
  auto startGate=[&](int mode,uint32_t selected,bool fail=false){gate.Reset();delayed->mode=mode;delayed->selected=selected;delayed->fail=fail;storageUiTest={};};
  auto done=[&]{Until([]{return !BackgroundCopies::Active();});delayed->mode=0;delayed->fail=false;Check(FileOperations::Pending().empty()&&VolumeCoord::Readable(base->ResourceKey()),"mutation leaked reservation");};
  auto phase=[&](HWND progress,const char* expected){char text[100]{};GetDlgItemTextA(progress,IDC_CP_PHASE,text,ARRAYSIZE(text));if(strcmp(text,expected))throw HfsPlus::Error(std::string("Expected mutation message: ")+expected+"; got: "+text);};
  auto dismiss=[&](HWND progress){if(IsWindow(progress))SendMessage(progress,WM_COMMAND,IDCANCEL,0);};
  // Slow backend rename returns immediately to the same browser. A queued
  // same-volume operation cannot run until rename commits and releases it.
  startGate(2,id);int row=Row(frame->GetBrowserSession(),L"Original");Check(frame->GetBrowserSession().MacBrowser().RenameItem(row,u"Renamed"),"start native background rename");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});HWND progress=BackgroundCopies::DialogWindow();char title[80]{};GetWindowTextA(progress,title,ARRAYSIZE(title));Check(!strcmp(title,"Renaming files")&&frame->IsWindowEnabled()&&Row(frame->GetBrowserSession(),L"Original")>=0,"rename blocked/optimistically changed browser");
  auto held=FileOperations::Pending().front().Job();FileOperations::Location same;same.SetMounted(delayed,id);same.expectedName=u"Original";FileOperations::Request queued=FileOperations::RenameEntry{same,u"MustNotRun"};queued.background=true;auto pending=FileOperations::Submit(queued);Check(!pending.Job()->done(),"same-volume mutation bypassed writer");pending.Job()->cancel();
  Host(frame->GetBrowserSession(),host);int independent=frame->GetBrowserSession().MacBrowser().AttachTestVolume(other,"other");Check(frame->GetBrowserSession().MacBrowser().ShowTestFolder(independent,2)&&frame->IsWindowEnabled(),"cannot browse another image during rename");auto next=app.CreateBrowser(FALSE);Check(next!=nullptr,"new browser during rename");auto& nextSession=next->GetBrowserSession();int shared=nextSession.MacBrowser().AttachTestVolume(delayed,"same");Check(nextSession.MacBrowser().ShowTestFolder(shared,2),"defer same-volume browser");HWND closed=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);Check(!IsWindow(closed)&&BackgroundCopies::ProgressOwners(next->GetSafeHwnd())==1&&!held->cancelled(),"closing rename owner canceled work");SetEvent(gate.release);done();Until([&]{return Row(nextSession,L"Renamed")>=0;});Check(!held->result().code&&base->Stat(id).name==u"Renamed"&&!Find(*base,u"MustNotRun")&&pending.Job()->result().error==ERROR_CANCELLED,"rename identity or queued cancellation failed");frame=next;mount=shared;
  fprintf(report,"PASS: slow native rename is non-modal, keeps old rows until completion, permits unrelated browsing/new windows, reserves its volume, survives source-window closure and refreshes a waiting browser.\n");
  // Cancellation during slow validation must prevent the atomic rename.
  Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show rename cancel source");startGate(1,id);Check(nextSession.MacBrowser().RenameItem(Row(nextSession,L"Renamed"),u"Canceled"),"queue cancel rename");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();SendMessage(progress,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);done();Check(base->Stat(id).name==u"Renamed"&&BackgroundCopies::LastResult().error==ERROR_CANCELLED,"cancel during metadata validation still renamed entry");phase(progress,"Rename cancelled");dismiss(progress);
  // Failure after background submission reports Rename, preserving catalog ID.
  Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show rename error source");startGate(2,id,true);Check(nextSession.MacBrowser().RenameItem(Row(nextSession,L"Renamed"),u"Failed"),"queue failed rename");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();SetEvent(gate.release);done();Check(BackgroundCopies::LastResult().code&&base->Stat(id).name==u"Renamed","failed rename changed source");phase(progress,"Rename failed");dismiss(progress);
  // Recursive deletion preflights before removing anything and remains
  // cancelable while that validation is stalled.
  auto folder=base->MakeDirectory(2,u"Folder");MacBrowser::Entry entry;entry.name=u"A";entry.forks[0].size=4;auto childA=base->Put(folder,entry,[](unsigned,uint64_t,void* p,size_t n){memset(p,'A',n);},false);entry.name=u"B";auto childB=base->Put(folder,entry,[](unsigned,uint64_t,void* p,size_t n){memset(p,'B',n);},false);Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show delete tree");Select(nextSession,L"Folder");startGate(3,folder);Check(nextSession.MacBrowser().Delete(false),"start cancel delete");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();Check(frame->IsWindowEnabled(),"delete disabled browser");Host(nextSession,host);SendMessage(progress,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);done();Check(BackgroundCopies::LastResult().error==ERROR_CANCELLED&&base->Stat(childA).parent==folder&&base->Stat(childB).parent==folder,"canceled validation deleted children");phase(progress,"Delete cancelled");dismiss(progress);
  // A partial failure refreshes the surviving entries rather than claiming
  // rollback of files that were already successfully removed.
  Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show partial-delete tree");Select(nextSession,L"Folder");startGate(4,childB,true);Check(nextSession.MacBrowser().Delete(false),"start partial delete");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(delayed->removed==1&&frame->IsWindowEnabled(),"recursive delete failed to make independent progress");progress=BackgroundCopies::DialogWindow();SetEvent(gate.release);done();Check(BackgroundCopies::LastResult().code&&base->List(folder).size()==1&&base->List(folder).front().id==childB&&base->Stat(childB).parent==folder&&base->Stat(folder).directory,"partial delete lost remaining entries");Check(BackgroundCopies::LastResult().partial,"delete failure did not report partial completion");phase(progress,"Delete failed (partial completion)");dismiss(progress);Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show remaining delete tree");Select(nextSession,L"Folder");Check(nextSession.MacBrowser().Delete(false),"retry delete");done();Check(!Find(*base,u"Folder"),"delete retry did not remove remaining folder");
  fprintf(report,"PASS: rename cancellation/error messages preserve the old name; recursive delete preflight cancellation preserves all children, partial failure retains remaining entries, and retry completes deletion.\n");
  // Windows label edits enqueue captured paths and retain the displayed label
  // until a confirmed result. Long names are not truncated to HFS's 31 bytes.
  CString windows=host+"\\windows",elsewhere=host+"\\elsewhere";Check(CreateDirectoryA(windows,nullptr)&&CreateDirectoryA(elsewhere,nullptr),"create mutation host folders");{CFile f;Check(f.Open(windows+"\\Old",CFile::modeCreate|CFile::modeWrite),"create rename host file");f.Write("HOST",4);} // Keep is created separately below.
  {CFile f;Check(f.Open(elsewhere+"\\Keep",CFile::modeCreate|CFile::modeWrite),"create unrelated file");f.Write("KEEP",4);}Host(nextSession,windows);Select(nextSession,L"Old");row=Row(nextSession,L"Old");Check(row>=0,"host rename row missing");auto reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::PathKey(VolumeCoord::Wide(std::string((LPCSTR)windows))),VolumeCoord::Mode::Write}});const char* longName="This_filename_is_longer_than_thirty_one_characters.dat";Check(nextSession.List()->TestCommitLabel(row,longName)==0&&BackgroundCopies::Active()&&Row(nextSession,L"Old")>=0,"Windows label edit did not stay non-modal/non-optimistic");Host(nextSession,elsewhere);reservation.reset();done();CString renamed=windows+"\\"+longName;Check(GetFileAttributesA(renamed)!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(windows+"\\Old")==INVALID_FILE_ATTRIBUTES,"Windows queued rename used a changed folder or truncated name");{auto hostVolume=MacBrowser::OpenWindows(VolumeCoord::Wide(std::string((LPCSTR)windows)));MacBrowser::Entry e;e.name=u"Forked";e.forks[0].size=4;e.forks[1].size=7;hostVolume->Put(2,e,[](unsigned fork,uint64_t,void* p,size_t n){memset(p,fork?'R':'D',n);},false);hostVolume->Close();}Host(nextSession,windows);Select(nextSession,L"Forked");row=Row(nextSession,L"Forked");Check(nextSession.List()->TestCommitLabel(row,"Forked renamed")==0,"queue companion rename");done();{auto resolved=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(std::string((LPCSTR)windows)+"\\Forked renamed"));Check(resolved.first->Stat(resolved.second).forks[1].size==7,"rename lost resource companion");char bytes[7]{};resolved.first->Read(resolved.second,1,0,bytes,7);Check(!memcmp(bytes,"RRRRRRR",7),"rename changed resource bytes");resolved.first->Close();}{HANDLE lock=CreateFileA(windows+"\\Forked renamed",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);Check(lock!=INVALID_HANDLE_VALUE,"lock Windows rename source");auto resolved=MacBrowser::ResolveWindowsFile(VolumeCoord::Wide(std::string((LPCSTR)windows)+"\\Forked renamed"));bool failed=false;try{auto e=resolved.first->Stat(resolved.second);resolved.first->Rename(resolved.second,e.parent,u"Blocked");}catch(const std::exception&){failed=true;}CloseHandle(lock);Check(failed&&GetFileAttributesA(windows+"\\Forked renamed")!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(windows+"\\%Forked renamed")!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(windows+"\\%Blocked")==INVALID_FILE_ATTRIBUTES,"failed Windows rename did not restore companion");resolved.first->Close();}Host(nextSession,windows);Select(nextSession,VolumeCoord::Wide(longName).c_str());reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::PathKey(VolumeCoord::Wide(std::string((LPCSTR)windows))),VolumeCoord::Mode::Write}});nextSession.List()->TestDeleteSelection();Check(BackgroundCopies::Active()&&GetFileAttributesA(renamed)!=INVALID_FILE_ATTRIBUTES,"Windows delete did not queue");Host(nextSession,elsewhere);reservation.reset();done();Check(GetFileAttributesA(renamed)==INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(elsewhere+"\\Keep")!=INVALID_FILE_ATTRIBUTES,"Windows delete changed wrong folder");
  {CFile f;Check(f.Open(windows+"\\Stale",CFile::modeCreate|CFile::modeWrite),"create stale host item");f.Write("OLD",3);}Host(nextSession,windows);Select(nextSession,L"Stale");reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::PathKey(VolumeCoord::Wide(std::string((LPCSTR)windows))),VolumeCoord::Mode::Write}});nextSession.List()->TestDeleteSelection();progress=BackgroundCopies::DialogWindow();Check(MoveFileA(windows+"\\Stale",windows+"\\Held"),"replace queued source");{CFile f;Check(f.Open(windows+"\\Stale",CFile::modeCreate|CFile::modeWrite),"create replacement host item");f.Write("NEW",3);}reservation.reset();done();Check(BackgroundCopies::LastResult().code&&GetFileAttributesA(windows+"\\Stale")!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesA(windows+"\\Held")!=INVALID_FILE_ATTRIBUTES,"queued delete removed replacement file");dismiss(progress);
  fprintf(report,"PASS: real Windows label/delete handlers enqueue captured paths, keep labels unchanged until refresh, retain long names, and reject a replaced queued source without deleting either entry.\n");
  // Classic HFS uses the same inline rename/delete entry points and normal
  // volume notifications, preserving both forks and catalog identity.
  CString classic=host+"\\classic.dsk";{CFile f;Check(f.Open(classic,CFile::modeCreate|CFile::modeWrite),"create classic mutation image");f.SetLength(4*1024*1024);}char label[]="BackgroundClassic";Check(hfs_format(const_cast<char*>((LPCSTR)classic),0,label)==0,"format classic mutation image");uint32_t catalog=0;{auto disk=MacBrowser::OpenClassic((LPCSTR)classic,"BackgroundClassic:",true);auto file=Put(*disk,u"Classic");catalog=disk->Stat(file).resolvedId;disk->Close();}auto doc=nextSession.Document();int index=doc->insert_volume(classic,0,0,0,0);Check(index>=0&&doc->Mounts().OpenClassicForTest(index),"open classic mutation browser");auto& v=doc->Mounts().ClassicAt(index);doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},nextSession.Tree()->initialize_volume(index,v.m_volume_name,2,0,0,0,0));nextSession.Tree()->GetTreeCtrl().SelectItem(nextSession.MountRoot({BrowserMountInventory::Kind::ClassicVolume,index}));Check(nextSession.AwaitNavigationForTest(),"open classic mutation folder");Select(nextSession,L"Classic");row=Row(nextSession,L"Classic");reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::ImageKey(std::string((LPCSTR)classic)),VolumeCoord::Mode::Write}});Check(nextSession.List()->TestCommitLabel(row,"Classic renamed")==0&&BackgroundCopies::Active(),"classic inline rename blocked");Host(nextSession,elsewhere);reservation.reset();done();{auto disk=MacBrowser::OpenClassic((LPCSTR)classic,"BackgroundClassic:",false);auto file=Find(*disk,u"Classic renamed");Check(file&&disk->Stat(file).resolvedId==catalog&&disk->Stat(file).forks[1].size==31,"classic rename changed catalog ID or lost fork");disk->Close();}nextSession.Tree()->GetTreeCtrl().SelectItem(nextSession.MountRoot({BrowserMountInventory::Kind::ClassicVolume,index}));Check(nextSession.AwaitNavigationForTest(),"return to classic mutation folder");Select(nextSession,L"Classic renamed");nextSession.List()->TestDeleteSelection();Check(BackgroundCopies::Active()&&frame->IsWindowEnabled(),"classic delete blocked");done();Until([&]{return Row(nextSession,L"Classic renamed")<0;});
  fprintf(report,"PASS: classic inline rename/delete run in background through captured image paths and catalog IDs, preserving resource forks and refreshing the browser. No physical media or computer control.\n");
  // Closing the final browser cancels a delete stalled during source validation
  // and leaves the frame alive until the worker can release its reservation.
  Check(nextSession.MacBrowser().ShowTestFolder(mount,2),"show final delete source");Select(nextSession,L"Renamed");startGate(1,id);Check(nextSession.MacBrowser().Delete(false),"start final-window delete");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto finalJob=FileOperations::Pending().front().Job();HWND last=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);Check(IsWindow(last)&&finalJob->cancelled()&&BackgroundCopies::Active(),"last browser did not safely cancel delete");SetEvent(gate.release);Until([&]{return app.m_browsers.empty();});Check(!IsWindow(last)&&!BackgroundCopies::Active()&&FileOperations::Pending().empty()&&VolumeCoord::Readable(base->ResourceKey())&&base->Stat(id).name==u"Renamed","last-window delete cleanup lost source or leaked reservation");
  fprintf(report,"PASS: last-window close cancels a stalled delete, waits for safe cleanup and retains its source; failed Windows data rename restores the resource companion.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last result: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 SetEvent(gate.release);BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunBackgroundMetadataTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\background-metadata-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;Gate gate;
 struct SlowMetadata:MacBrowser::Volume {
  std::shared_ptr<MacBrowser::Volume> base;Gate& gate;std::atomic<int> mode{0};bool fail=false;
  SlowMetadata(std::shared_ptr<MacBrowser::Volume> b,Gate& g):base(b),gate(g){}
  void Hold(int at){if(mode==at){SetEvent(gate.entered);Check(WaitForSingleObject(gate.release,15000)==WAIT_OBJECT_0,"metadata gate timed out");if(fail)throw HfsPlus::Error("Injected metadata failure");}}
  std::string ResourceKey()const override{return base->ResourceKey();}std::u16string Name()const override{return base->Name();}const char* Kind()const override{return base->Kind();}bool Writable()const override{return base->Writable();}bool CaseSensitive()const override{return base->CaseSensitive();}uint64_t FreeBytes()const override{return base->FreeBytes();}void Reload()override{base->Reload();}
  MacBrowser::Entry Stat(uint32_t id)override{
   int busy=4;
   if(mode.compare_exchange_strong(busy,0))throw VolumeCoord::Busy();
   Hold(1);
   return base->Stat(id);
  }
  std::vector<MacBrowser::Entry> List(uint32_t id)override{return base->List(id);}void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override{if(n)Hold(3);base->Read(id,fork,at,p,n);}
  uint32_t MakeDirectory(uint32_t id,const std::u16string& name)override{Hold(2);return base->MakeDirectory(id,name);}uint32_t Put(uint32_t id,const MacBrowser::Entry& e,MacBrowser::Reader r,bool replace)override{return base->Put(id,e,r,replace);}void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{base->Rename(id,parent,name);}void Remove(uint32_t id)override{base->Remove(id);}
 };
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create metadata browser");auto base=Image(host+"\\metadata.dsk"),input=Image(host+"\\input.dsk");auto slow=std::make_shared<SlowMetadata>(base,gate);auto file=Put(*base,u"File"),incoming=Put(*input,u"Input");auto& session=frame->GetBrowserSession();int mount=session.MacBrowser().AttachTestVolume(slow,"metadata");Check(session.MacBrowser().ShowTestFolder(mount,2),"show metadata volume");
  auto start=[&](int mode,bool fail=false){gate.Reset();slow->mode=mode;slow->fail=fail;storageUiTest={};};
  auto finished=[&]{Until([]{return !BackgroundCopies::Active();});slow->mode=0;slow->fail=false;Check(FileOperations::Pending().empty()&&VolumeCoord::Readable(base->ResourceKey()),"metadata job leaked reservations");};
  auto dismiss=[&](HWND window){if(IsWindow(window))SendMessage(window,WM_COMMAND,IDCANCEL,0);};
  auto outcome=[&](HWND window,const char* text){char actual[100]{};GetDlgItemTextA(window,IDC_CP_PHASE,actual,ARRAYSIZE(actual));Check(!strcmp(actual,text),"metadata job has wrong completion label");};
  FileOperations::Location target;target.SetMounted(slow,2);
  start(2);Check(session.MacBrowser().CreateFolder(target,u"Created"),"start slow folder creation");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(frame->IsWindowEnabled(),"folder creation disabled browser");Host(session,host);SetEvent(gate.release);finished();Check(Find(*base,u"Created"),"folder creation lost captured target");Check(session.MacBrowser().ShowTestFolder(mount,2)&&Row(session,L"Created")>=0,"folder creation did not refresh");
  start(2,true);Check(session.MacBrowser().CreateFolder(target,u"Failed"),"start failed folder creation");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});HWND progress=BackgroundCopies::DialogWindow();SetEvent(gate.release);finished();Check(BackgroundCopies::LastResult().code&&!Find(*base,u"Failed"),"failed creation left a folder");outcome(progress,"Folder creation failed");dismiss(progress);
  start(1);Check(session.MacBrowser().CreateFolder(target,u"Canceled"),"start canceled creation");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();SendMessage(progress,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);finished();Check(BackgroundCopies::LastResult().error==ERROR_CANCELLED&&!Find(*base,u"Canceled"),"canceled validation still created folder");dismiss(progress);
  fprintf(report,"PASS: slow folder creation stays non-modal and uses the captured target; completion refreshes, failures/cancellation leave no folder and release reservations.\n");
  Check(session.MacBrowser().ShowTestFolder(mount,2),"show delayed Properties source");
  {
   auto item=target;
   item.SetId(file);
   start(4);
   FileOperations::Request inspect=FileOperations::InspectProperties{item};
   inspect.background=true;
   auto reading=FileOperations::Submit(inspect);
   Until([&]{return reading.Job()->done();});
   Check(!reading.Job()->result().code&&reading.Snapshot().entry.id==file&&slow->mode==0,
         "Properties did not retry a transient shared-engine read conflict");
   fprintf(report,"PASS: Properties retries an engine read conflict under its shared lease.\n");
  }
  start(1);
  Select(session,L"File");
  CMenu propertiesMenu;
  Check(propertiesMenu.CreatePopupMenu() &&
        propertiesMenu.AppendMenu(MF_STRING, ID_OWN_FILE_PROPERTIES, "Properties..."),
        "create Properties menu");
  for (UINT command : {ID_OWN_FILE_OPEN, ID_OWN_NEW_FOLDER, ID_OWN_DELETE})
   Check(propertiesMenu.AppendMenu(MF_STRING, command, "Browser command"), "add browser command");
  auto menuEnabled = [&](UINT command) {
   frame->SendMessage(WM_INITMENUPOPUP, WPARAM(propertiesMenu.GetSafeHmenu()), 0);
   return (propertiesMenu.GetMenuState(command, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0;
  };
  auto propertiesEnabled = [&] { return menuEnabled(ID_OWN_FILE_PROPERTIES); };
  Check(menuEnabled(ID_OWN_FILE_OPEN) && menuEnabled(ID_OWN_NEW_FOLDER) && menuEnabled(ID_OWN_DELETE),
        "frame disabled valid HFS+ browser commands");
  Check(propertiesEnabled(), "frame disabled Properties for a selected HFS+ file");
  session.List()->GetListCtrl().SetItemState(-1, 0, LVIS_SELECTED);
  Check(!propertiesEnabled() && !menuEnabled(ID_OWN_FILE_OPEN) && !menuEnabled(ID_OWN_DELETE)
        && menuEnabled(ID_OWN_NEW_FOLDER), "popup used stale selection");
  Select(session,L"File");
  Check(propertiesEnabled(), "frame did not re-enable Properties after selection");
  BackgroundCopies::enabled=false;
  frame->SendMessage(WM_COMMAND,ID_OWN_FILE_PROPERTIES);
  BackgroundCopies::enabled=true;
  Check(BackgroundCopies::Active(),"Properties command did not use the typed background job");
  Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});
  Host(session,host);
  Check(frame->IsWindowEnabled() && !MetadataDialogs::Window(),"navigation blocked by Properties");
  SetEvent(gate.release);
  finished();
  auto capturedProperties=MetadataDialogs::Window();
  Check(IsWindow(capturedProperties) &&
        BrowserPresentation::Text(GetDlgItem(capturedProperties,IDC_CP_PHASE))==L"File",
        "Delayed Properties used the new folder instead of its captured entry");
  Check(BrowserPresentation::Text(GetDlgItem(capturedProperties,IDC_PROP_DATA_SIZE))==L"8 KiB (8,192 bytes)"&&
        BrowserPresentation::Text(GetDlgItem(capturedProperties,IDC_PROP_RESOURCE_SIZE))==L"31 bytes"&&
        BrowserPresentation::Text(GetDlgItem(capturedProperties,IDCANCEL))==L"Close",
        "Properties did not show the exact sizes of both forks and the Close action");
  dismiss(capturedProperties);
  Check(session.MacBrowser().ShowTestFolder(mount,2),"show properties source");start(1);Check(session.MacBrowser().Properties(Row(session,L"File")),"start slow Properties");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(frame->IsWindowEnabled()&&!MetadataDialogs::Window(),"properties blocked browser or opened before metadata");auto next=app.CreateBrowser(FALSE);Check(next!=nullptr,"new browser during properties");HWND old=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);Check(!IsWindow(old)&&BackgroundCopies::Active(),"properties owner cannot close");Host(next->GetBrowserSession(),host);SetEvent(gate.release);finished();HWND properties=MetadataDialogs::Window();Check(IsWindow(properties)&&next->IsWindowEnabled(),"modeless Properties lost after owner closure");Check(BrowserPresentation::Text(GetDlgItem(properties,IDC_CP_PHASE))==L"File","Properties retargeted after navigation");dismiss(properties);frame=next;
  FileOperations::Location selected;selected.SetMounted(slow,file);selected.expectedName=u"File";start(1);Check(MetadataDialogs::Show(selected,frame),"start canceled Properties");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();SendMessage(progress,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);finished();Check(!MetadataDialogs::Window(),"canceled inspection opened Properties");outcome(progress,"Properties read cancelled");dismiss(progress);
  start(1,true);Check(MetadataDialogs::Show(selected,frame),"start failed Properties");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();SetEvent(gate.release);finished();Check(!MetadataDialogs::Window(),"failed inspection opened Properties");outcome(progress,"Properties read failed");dismiss(progress);
  fprintf(report,"PASS: slow/error/canceled Properties reads are background jobs; modeless results use captured metadata and survive closing the submitting browser.\n");
  {auto inputSource=std::make_shared<SlowMetadata>(input,gate);FileOperations::Location property;property.SetMounted(base,file);
   for(unsigned outcome=0;outcome<4;++outcome){auto before=base->GetAttributes(file);Check(MetadataDialogs::Show(property,frame),"open import editor");finished();auto summary=MetadataDialogs::Window();SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto editor=MetadataDialogs::Window();gate.Reset();inputSource->mode=3;inputSource->fail=outcome==1;FileOperations::Location source;source.SetMounted(inputSource,incoming);auto name=u"user.Import"+MacBrowser::U16(std::to_wstring(outcome));Check(MetadataDialogs::ImportValue(editor,source,name),"start editor import");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto progress=BackgroundCopies::DialogWindow();Check(frame->IsWindowEnabled()&&!IsWindowEnabled(GetDlgItem(editor,IDOK)),"pending import did not coordinate Apply");SendMessage(editor,WM_COMMAND,IDOK,0);Check(IsWindow(editor)&&base->GetAttributes(file)==before,"Apply ran before import completed");if(outcome==2)SendMessage(progress,WM_COMMAND,IDCANCEL,0);if(outcome==3)SendMessage(editor,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);finished();inputSource->mode=0;inputSource->fail=false;
    if(outcome==0){Check(IsWindowEnabled(GetDlgItem(editor,IDOK)),"successful import left Apply disabled");SendMessage(editor,WM_COMMAND,IDOK,0);finished();Check(base->GetAttributes(file).at(name).size()==8192,"Apply lost the completed import");}
    else{Check(base->GetAttributes(file)==before,"failed/cancelled/closed import changed attributes");if(outcome<3)Check(IsWindowEnabled(GetDlgItem(editor,IDOK)),"failed import left editor disabled");else Check(!IsWindow(editor),"late import revived a closed editor");}dismiss(progress);MetadataDialogs::Shutdown();
   }
  }
  for(unsigned kind=0;kind<4;++kind){auto token=std::make_shared<int>(7);std::weak_ptr<int> weak=token;FileOperations::CreateFolder request{};request.parent.SetMounted(base,2);request.name=u"Callback"+MacBrowser::U16(std::to_wstring(kind));auto name=request.name;Check(BackgroundCopies::Start({request},frame,true,{},[token,kind](const FileOperations::Completion&){if(kind==0)throw std::runtime_error("Injected completion error");if(kind==1)AfxThrowUserException();if(kind==3)throw std::runtime_error("");throw 7;}),"start throwing completion");auto progress=BackgroundCopies::DialogWindow();token.reset();finished();Check(!BackgroundCopies::LastResult().code&&Find(*base,name)&&frame->IsWindowEnabled(),"UI callback exception changed operation success");Check(weak.expired(),"failed completion retained its callback captures");outcome(progress,"Folder creation completed; completion display failed");SendMessage(progress,WM_TIMER,1,0);SendMessage(progress,WM_TIMER,1,0);Check(!BackgroundCopies::Active()&&Find(*base,name),"Stale completion timer restarted or corrupted a finished job");dismiss(progress);}
  fprintf(report,"PASS: editor Apply waits for attribute import; success, failure, cancellation and close-during-import restore controls or discard late delivery safely. Standard/MFC/unknown completion exceptions stay in the progress window without changing successful operation results or retaining captures.\n");
  {auto window=std::make_shared<HWND>(nullptr);FileOperations::CreateFolder first{};first.parent.SetMounted(base,2);first.name=u"ReentrantFirst";auto second=first;second.name=u"ReentrantSecond";bool started=false;Check(BackgroundCopies::Start({first},frame,true,{},[&,window,second](const FileOperations::Completion&){SendMessage(*window,WM_COMMAND,IDCANCEL,0);started=BackgroundCopies::Start({second},frame,true);}),"start reentrant completion");*window=BackgroundCopies::DialogWindow();finished();Check(started&&!IsWindow(*window)&&Find(*base,u"ReentrantFirst")&&Find(*base,u"ReentrantSecond")&&frame->IsWindowEnabled(),"Closing/replacing a progress window during completion was unsafe");}
  fprintf(report,"PASS: stale progress timers cannot re-finish jobs; closing a completed progress window and starting another job from its callback preserves object lifetime and both operations.\n");
  // Replacement imports preserve the old fork on cancellation/read error.
  auto slowInput=std::make_shared<SlowMetadata>(input,gate);FileOperations::ImportResourceFork replace;FileOperations::Location from;from.SetMounted(slowInput,incoming);replace={from,selected};
  auto originalFork=HfsPlus::Bytes(31);base->Read(file,1,0,originalFork.data(),originalFork.size());
  for(bool failure:{false,true}){gate.Reset();slowInput->mode=3;slowInput->fail=failure;Check(BackgroundCopies::Start({replace},frame,true),"start background fork replacement");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});progress=BackgroundCopies::DialogWindow();Check(frame->IsWindowEnabled(),"resource import disabled browser");if(!failure)SendMessage(progress,WM_COMMAND,IDCANCEL,0);SetEvent(gate.release);finished();Check(BackgroundCopies::LastResult().code&&base->Stat(file).forks[1].size==31&&base->Stat(file).forks[0].size==8192,"resource failure lost original forks");auto preserved=HfsPlus::Bytes(31);base->Read(file,1,0,preserved.data(),preserved.size());Check(preserved==originalFork,"Failed resource import changed the original fork bytes");outcome(progress,failure?"Resource import failed":"Resource import cancelled");dismiss(progress);}
  slowInput->mode=0;Check(BackgroundCopies::Start({replace},frame,true),"start successful resource import");finished();Check(!BackgroundCopies::LastResult().code&&base->Stat(file).forks[1].size==8192&&base->Stat(file).forks[0].size==8192,"background resource replacement failed");
  fprintf(report,"PASS: background resource-fork imports preserve both old forks on read failure/cancellation and replace only the resource fork on success.\n");
  auto& surviving=frame->GetBrowserSession();CString windows=host+"\\windows";Check(CreateDirectoryA(windows,nullptr),"create Windows folder fixture");Host(surviving,windows);FileOperations::Location hostTarget;hostTarget.SetWindows(VolumeCoord::Wide((LPCSTR)windows));auto reserved=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::PathKey(hostTarget.WindowsPath()),VolumeCoord::Mode::Write}});Check(surviving.MacBrowser().CreateFolder(hostTarget,u"Captured"),"queue Windows folder creation");Host(surviving,host);reserved.reset();finished();Check(GetFileAttributesA(windows+"\\Captured")!=INVALID_FILE_ATTRIBUTES&&(GetFileAttributesA(windows+"\\Captured")&FILE_ATTRIBUTE_DIRECTORY),"Windows creation used changed navigation");{CFile marker;Check(marker.Open(windows+"\\Captured\\Keep",CFile::modeCreate|CFile::modeWrite),"create duplicate-folder preservation fixture");marker.Write("KEEP",4);marker.Close();}Check(surviving.MacBrowser().CreateFolder(hostTarget,u"Captured"),"queue duplicate folder creation");progress=BackgroundCopies::DialogWindow();finished();Check(BackgroundCopies::LastResult().code&&GetFileAttributesA(windows+"\\Captured\\Keep")!=INVALID_FILE_ATTRIBUTES,"duplicate folder replaced the existing folder");dismiss(progress);
  hostTarget.expectedIdentity=MacBrowser::DescribeWindowsFile(hostTarget.WindowsPath()).identity;reserved=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::PathKey(hostTarget.WindowsPath()),VolumeCoord::Mode::Write}});Check(surviving.MacBrowser().CreateFolder(hostTarget,u"MustNotCreate"),"queue replaced destination");progress=BackgroundCopies::DialogWindow();Check(MoveFileA(windows,windows+"-held")&&CreateDirectoryA(windows,nullptr),"replace queued destination folder");reserved.reset();finished();Check(BackgroundCopies::LastResult().code&&GetFileAttributesA(windows+"\\MustNotCreate")==INVALID_FILE_ATTRIBUTES,"queued create changed replacement directory");dismiss(progress);
  fprintf(report,"PASS: Windows folder creation queues under host-volume coordination and retains its captured path after navigation; duplicates fail without replacing the folder.\n");
  CString classic=host+"\\classic.dsk";{CFile f;Check(f.Open(classic,CFile::modeCreate|CFile::modeWrite),"create metadata HFS image");f.SetLength(1474560);}char label[]="MetadataClassic";Check(hfs_format(const_cast<char*>((LPCSTR)classic),0,label)==0,"format metadata HFS image");{auto v=MacBrowser::OpenClassic((LPCSTR)classic,"MetadataClassic:",true);Put(*v,u"Classic");v->Close();}
  FileOperations::Location legacy;legacy.SetClassic((LPCSTR)classic,"MetadataClassic:Classic");
  auto submit=[&](FileOperations::Request r,bool pass=true){r.background=true;auto operation=FileOperations::Submit(r);Until([&]{return operation.Job()->done();});if((operation.Job()->result().code==0)!=pass){fprintf(report,"Unexpected metadata result: kind=%d expected=%d code=%d message=%s\n",int(r.OperationKind()),pass,operation.Job()->result().code,operation.Job()->result().message.c_str());fflush(report);}Check((operation.Job()->result().code==0)==pass,"Metadata request returned an unexpected result");return operation.Snapshot();};
  FileOperations::InspectProperties inspect{legacy};auto initial=submit(inspect);Check(initial.classicProperties&&initial.capabilities.CanEditClassicProperties(),"classic inspect lost editable properties");legacy.expectedCatalogId=initial.entry.resolvedId;
  FileOperations::EditClassicProperties patch{legacy,FileOperations::FinderLabel{3}};submit(patch);auto changed=submit(inspect);Check((changed.properties.fdflags&14)==6&&((changed.properties.fdflags^initial.properties.fdflags)&~14)==0,"label update changed other flags");patch.change=FileOperations::FinderPosition{123,-25};submit(patch);changed=submit(inspect);Check(changed.properties.fdlocation.h==123&&changed.properties.fdlocation.v==-25&&(changed.properties.fdflags&14)==6,"position update lost label");
  FileOperations::EditClassicProperties edit{legacy,FileOperations::ClassicPropertyEdit{initial.properties,initial.properties}};auto& propertyEdit=std::get<FileOperations::ClassicPropertyEdit>(edit.change);propertyEdit.desired.fdflags|=HFS_FNDR_ISINVISIBLE;submit(edit,false);Check(submit(inspect).properties.fdlocation.h==123,"stale editor overwrote newer metadata");
  FileOperations::CreateFolder mkdir{};mkdir.parent=legacy;mkdir.parent.SetClassic(mkdir.parent.ClassicImage(),"MetadataClassic:");mkdir.parent.expectedCatalogId=2;mkdir.name=u"Folder";submit(mkdir);submit(mkdir,false);{auto v=MacBrowser::OpenClassic((LPCSTR)classic,"MetadataClassic:",false);Check(Find(*v,u"Folder"),"classic folder missing");v->Close();}
  Check(MetadataDialogs::Show(legacy,frame),"open classic modeless editor");finished();properties=MetadataDialogs::Window();Check(IsWindow(properties)&&IsWindowEnabled(GetDlgItem(properties,IDC_PROP_TYPE)),"classic editor missing");SetDlgItemTextA(properties,IDC_PROP_NAME,"Edited");SetDlgItemTextA(properties,IDC_PROP_CREATOR,"TEST");SendMessage(properties,WM_COMMAND,IDOK,0);Check(BackgroundCopies::Active()&&!IsWindow(properties),"classic editor did not queue update");finished();Check(!BackgroundCopies::LastResult().code,"classic editor failed");auto renamed=legacy;renamed.SetClassic(renamed.ClassicImage(),"MetadataClassic:Edited");inspect.item=renamed;changed=submit(inspect);Check(changed.properties.cnid==initial.properties.cnid&&!memcmp(changed.properties.u.file.creator,"TEST",4)&&changed.properties.u.file.rsize==31&&changed.properties.u.file.dsize==8192,"classic metadata edit lost catalog identity/forks");
  // A duplicate rename must not apply its accompanying metadata change.
  edit.item=renamed;propertyEdit={changed.properties,changed.properties};strcpy_s(propertyEdit.desired.name,"Folder");memcpy(propertyEdit.desired.u.file.creator,"FAIL",4);submit(edit,false);Check(!memcmp(submit(inspect).properties.u.file.creator,"TEST",4),"failed metadata transaction changed creator");
  fprintf(report,"PASS: classic folder creation, patch-only label/Finder position updates, stale Properties rejection, modeless editor apply with rename, preserved catalog IDs/forks and failed-rename transaction rollback.\n");
#ifdef HFV_FLOPPY_TESTING
  {
   std::vector<BYTE> bytes(1474560);
   {CFile f;Check(f.Open(classic,CFile::modeRead)&&f.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"load metadata floppy");}
   TestFloppy disk(bytes);
   readonly_cd_backend_init();
   Check(readonly_floppy_attach_api(25,&disk),"attach metadata floppy");
   struct Detach {
    BrowserMountInventory* mounts=nullptr;
    int index=-1;
    ~Detach(){if(mounts)mounts->CloseClassic(index);readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}
   } detach;
   auto doc=surviving.Document();
   char device[64];
   get_floppy_volume_file_name(25,device);
   const int index=doc->insert_volume(device,1,0,0,0);
   Check(index>=0,"insert metadata floppy in the browser");
   auto& mounted=doc->Mounts().ClassicAt(index);
   detach.mounts=&doc->Mounts();
   detach.index=index;
   Check(doc->Mounts().OpenClassicForTest(index),"mount metadata floppy in the browser");
   auto tree=surviving.Tree();
   doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::ClassicVolume,index},tree->initialize_volume(index,mounted.m_volume_name,2,1,0,0,0));
   const auto notifications=tree->SelectionNotificationsEnabled();
   tree->enable_selchange(0);
   tree->GetTreeCtrl().SelectItem(doc->GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,index}));
   tree->enable_selchange(notifications);
   doc->open_sub_directory(index,2,mounted.m_volume_name);
   mkdir.parent=surviving.MacBrowser().TargetFolder();
   Check(mkdir.parent.IsPhysical()&&mkdir.parent.MediaGenerationKnown(),"folder creation lost its captured floppy generation");
   auto swappedFolder=mkdir;
   swappedFolder.parent.BindMediaGeneration(mkdir.parent.MediaGeneration()^1u);
   swappedFolder.name=u"MustNotCreate";
   const auto beforeFolder=disk.disk.media;
   submit(swappedFolder,false);
   Check(disk.disk.media==beforeFolder,"stale folder request changed floppy media");
   mkdir.name=u"Physical";
   submit(mkdir);
   Until([&]{return Row(surviving,L"Physical")>=0;});

   Select(surviving,L"Edited");
   auto item=surviving.MacBrowser().CaptureItem(Row(surviving,L"Edited"));
   Check(item.IsPhysical()&&item.MediaGenerationKnown(),"metadata command lost its captured floppy generation");
   BackgroundCopies::enabled=false;
   surviving.List()->SendMessage(WM_COMMAND,ID_OWN_SHORTCUT_COLOR_HOT);
   BackgroundCopies::enabled=true;
   Check(BackgroundCopies::Active(),"Finder label did not queue a typed job");
   finished();
   Check(!BackgroundCopies::LastResult().code,"Finder label command failed");
   inspect.item=item;
   auto physical=submit(inspect);
   Check((physical.properties.fdflags&14)==2*FINDER_COLOR_HOT&&physical.capabilities.CanEditClassicProperties(),"Finder label did not persist");

   surviving.finderLayout=true;
   surviving.List()->set_style(LVS_ICON);
   surviving.List()->set_origo(FINDER_MAGIC_OFFSET,FINDER_MAGIC_OFFSET);
   Select(surviving,L"Edited");
   auto& list=surviving.List()->GetListCtrl();
   const int row=Row(surviving,L"Edited");
   auto* finderFrame=surviving.Owner();
   const UINT initialDpi=NativeDpi::Window(finderFrame->GetSafeHwnd());
   RECT frameBounds{};
   finderFrame->GetWindowRect(&frameBounds);
   auto changeDpi=[&](UINT dpi) {
    finderFrame->SendMessage(NativeDpi::Changed,MAKEWPARAM(dpi,dpi),LPARAM(&frameBounds));
   };
   changeDpi(144);
   const CPoint startPosition(NativeDpi::Scale(70,144),NativeDpi::Scale(80,144));
   Check(list.SetItemPosition(row,startPosition),"place initial Finder icon");
   changeDpi(192);
   CPoint scaledPosition;
   Check(list.GetItemPosition(row,&scaledPosition)&&scaledPosition==CPoint(140,160),
         "DPI change altered the Finder icon's logical position");
   const CSize displacement(NativeDpi::Scale(12,192),NativeDpi::Scale(16,192));
   BackgroundCopies::enabled=false;
   surviving.List()->PlaceSelectedIconsForTest(displacement);
   BackgroundCopies::enabled=true;
   Check(BackgroundCopies::Active(),"Finder icon placement did not queue a typed job");
   finished();
   Check(!BackgroundCopies::LastResult().code,"Finder location command failed");
   physical=submit(inspect);
   Check(physical.properties.fdlocation.h==82&&physical.properties.fdlocation.v==96&&
         (physical.properties.fdflags&14)==2*FINDER_COLOR_HOT&&disk.writes&&!disk.locked,
         "Finder icon placement changed unrelated metadata or left the floppy locked");
   changeDpi(initialDpi);
   surviving.finderLayout=false;
   fprintf(report,"PASS: browser Finder label and icon placement use typed jobs on a simulated physical floppy and preserve other metadata.\n");
  }
#endif
  // Final-window close cancels a queued create and waits for validation cleanup.
  int lastMount=surviving.MacBrowser().AttachTestVolume(slow,"last-metadata");Check(surviving.MacBrowser().ShowTestFolder(lastMount,2),"show final metadata source");start(1);Check(surviving.MacBrowser().CreateFolder(target,u"MustNotExist"),"start final metadata operation");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto pending=FileOperations::Pending().front().Job();HWND last=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);Check(IsWindow(last)&&pending->cancelled(),"last-window metadata close did not cancel safely");SetEvent(gate.release);Until([&]{return app.m_browsers.empty();});Check(!Find(*base,u"MustNotExist")&&FileOperations::Pending().empty(),"last-window metadata cleanup failed");
  fprintf(report,"PASS: last-window close cancels stalled folder creation before mutation and drains pending jobs safely.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last result: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 SetEvent(gate.release);BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunPerVolumeCommandsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\per-volume-commands-report.txt","w");if(!report)return 2;int result=0;Gate stalled;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create command-policy browser");auto a=Image(host+"\\busy-a.dsk"),b=Image(host+"\\busy-b.dsk"),c=Image(host+"\\idle.dsk");auto slow=std::make_shared<Slow>(a,stalled);auto id=Put(*a,u"Slow");FileOperations::Request copy;FileOperations::Location from,to;from.SetMounted(slow,id);to.SetMounted(b);copy.Copy().sources={from};copy.Copy().destination=to;Check(BackgroundCopies::Start({copy},frame,true),"start policy copy");Until([&]{return WaitForSingleObject(stalled.entered,0)==WAIT_OBJECT_0;});
  auto& session=frame->GetBrowserSession();int idle=session.MacBrowser().AttachTestVolume(c,"idle"),busy=session.MacBrowser().AttachTestVolume(slow,"busy");Check(session.MacBrowser().ShowTestFolder(idle,2),"browse idle volume");
  for(UINT command:{UINT(ID_FILE_OPEN),UINT(ID_VIEW_OPTIONS),UINT(ID_FILE_NEWVOLUME),UINT(ID_FILE_NEWHFSPLUS),UINT(ID_TOOLS_BURN),UINT(ID_TOOLS_DUMPCDDEBUG),UINT(ID_FILE_VOLUMETOFLOPPY),UINT(ID_FILE_VOLUMEFROMFLOPPY),UINT(ID_TOOLS_FIND),UINT(ID_VIEW_MAC_WINDOW)})Check(frame->CommandAllowed(command),"unrelated command still globally disabled");
  auto other=app.CreateBrowser(FALSE);Check(other!=nullptr,"create independent command browser");auto& second=other->GetBrowserSession();int busySecond=second.MacBrowser().AttachTestVolume(slow,"busy-second");auto tree=second.Tree();HTREEITEM busyRoot=nullptr;for(auto item=tree->m_ct->GetRootItem();item;item=tree->m_ct->GetNextSiblingItem(item)){const auto* node=tree->BrowserNodeAt(item);if(node&&node->mount.kind==BrowserMountInventory::Kind::MountedVolume&&node->mount.index==busySecond){busyRoot=item;break;}}Check(busyRoot!=nullptr,"find busy root");tree->enable_selchange(0);tree->m_ct->SelectItem(busyRoot);tree->enable_selchange(1);Check(!second.List()->StorageSelectionAvailable()&&session.List()->StorageSelectionAvailable()&&!other->CommandAllowed(ID_TOOLS_FIND)&&!other->CommandAllowed(ID_VIEW_MAC_WINDOW)&&frame->CommandAllowed(ID_TOOLS_FIND),"selected-volume command routing leaked across windows");Check(other->CommandAllowed(ID_VIEW_OPTIONS)&&other->CommandAllowed(ID_FILE_OPEN),"target picker/settings depend on busy selection");
  struct Menu:CCmdUI{bool enabled=false;void Enable(BOOL value)override{enabled=value!=FALSE;}void SetCheck(int)override{}void SetRadio(BOOL)override{}}menu;
  // Physical command UI reflects readers as well as writers. No real drive is opened.
  auto doc=session.Document();
  const auto addOpticalRoot=[&](char letter){
   auto& mounts=doc->Mounts();
   const auto index=mounts.AddWindows(letter,DRIVE_CDROM);
   const auto& drive=mounts.WindowsAt(index);
   const auto root=session.Tree()->initialize_fat_volume(index,&drive.m_volume_name,drive.m_type,&drive.m_volume_name,FALSE);
   session.SetMountRoot({BrowserMountInventory::Kind::WindowsDrive,index},root);
   return root;
  };
  const auto firstOptical=addOpticalRoot('Y');
  const auto secondOptical=addOpticalRoot('Z');
  session.Tree()->enable_selchange(0);
  session.Tree()->m_ct->SelectItem(firstOptical);
  auto device24=VolumeCoord::DeviceKey(24),device25=VolumeCoord::DeviceKey(25);auto reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Read}});menu.m_nID=ID_FILE_EJECT;Check(frame->OnCmdMsg(ID_FILE_EJECT,CN_UPDATE_COMMAND_UI,&menu,nullptr)&&!menu.enabled&&!frame->CommandAllowed(ID_FILE_RELOAD),"eject/reload ignored active reader");session.Tree()->m_ct->SelectItem(secondOptical);menu.enabled=false;frame->OnCmdMsg(ID_FILE_EJECT,CN_UPDATE_COMMAND_UI,&menu,nullptr);Check(menu.enabled&&frame->CommandAllowed(ID_FILE_RELOAD),"unrelated drive eject/reload remained disabled");session.Tree()->m_ct->SelectItem(firstOptical);session.Tree()->enable_selchange(1);reservation.reset();
  fprintf(report,"PASS: real per-window command predicates and eject/reload update routing distinguish busy/read-reserved volumes from unrelated disks; target pickers and Options remain available during an active copy.\n");
  auto run=[&](std::function<Storage::Result(Storage::Job&)> work){auto job=Storage::worker().submit(std::move(work));Until([&]{return job->done();});return job->result();};
  struct CommandControl:MediaControlApi{unsigned types=0,opens=0,controls=0;UINT DriveType(int)override{++types;return DRIVE_CDROM;}HANDLE Open(int,DWORD)override{++opens;return (HANDLE)uintptr_t(5);}bool Control(HANDLE,DWORD)override{++controls;return true;}bool Close(HANDLE)override{return true;}}control;
  bool released=false;auto pendingEject=MakeMediaControlJob({24,DRIVE_CDROM},false,&control,[&]{released=true;});reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Read}});auto blocked=run(pendingEject);Check(blocked.error==ERROR_BUSY&&!released&&!control.types&&!control.opens,"eject touched/released busy device");auto independent=run(MakeMediaControlJob({25,DRIVE_CDROM},false,&control,[&]{released=true;}));Check(!independent.code&&released&&control.opens==1&&BackgroundCopies::Active(),"unrelated eject cannot run during copy");reservation.reset();Check(!run(pendingEject).code,"eject did not recover after reservation release");
  struct Source:MediaImageSource {unsigned opens=0,reads=0,closes=0;int drive=0;bool closedReserved=false;DWORD error=0;bool fail=false;bool Open(int value,std::string&)override{drive=value;++opens;return true;}void Close()override{++closes;closedReserved=!VolumeCoord::Available({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Write}});}unsigned long long Size()const override{return 4096;}bool Verify(DWORD& generation)override{generation=9;return true;}bool Read(unsigned long long,DWORD count,void* p,DWORD& got)override{++reads;if(fail){error=ERROR_READ_FAULT;return false;}memset(p,42,count);got=count;return true;}DWORD Error()const override{return error;}};
  auto raw=std::make_shared<Source>();std::string output=std::string((LPCSTR)host)+"\\dump.iso";auto imageJob=MakeMediaImageJob(24,output,raw,"Test");reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Write}});blocked=run(imageJob);Check(blocked.error==ERROR_BUSY&&!raw->opens&&GetFileAttributesA(output.c_str())==INVALID_FILE_ATTRIBUTES,"imaging accessed busy source or created output");reservation.reset();auto hostKey=VolumeCoord::PathKey(VolumeCoord::Wide(output));reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{hostKey,VolumeCoord::Mode::Write}});blocked=run(imageJob);Check(blocked.error==ERROR_BUSY&&!raw->opens,"imaging ignored busy destination");reservation.reset();Check(!run(imageJob).code&&raw->reads&&raw->closes==1&&raw->closedReserved&&BackgroundCopies::Active(),"unrelated imaging failed");raw->fail=true;auto failed=run(MakeMediaImageJob(25,std::string((LPCSTR)host)+"\\failed.iso",raw,"Test"));Check(failed.code&&VolumeCoord::Available({{device25,VolumeCoord::Mode::Write},{hostKey,VolumeCoord::Mode::Write}}),"image error leaked reservations");
  fprintf(report,"PASS: eject/reload and raw imaging recheck selected resources in the worker; busy sources/destinations are rejected before API calls, output creation or reader release, while unrelated operations succeed and errors release reservations.\n");
  // Source image and recorder aliases are protected throughout prepare and burn.
  struct Burner:CdBurnBackend{std::string key;unsigned opens=0,writes=0;explicit Burner(std::string k):key(std::move(k)){}std::vector<std::string> Resources(const std::wstring&)override{return {key};}HRESULT Open(const std::wstring&)override{++opens;return S_OK;}HRESULT Inspect(CdBurnMedia& media)override{media={true,true,1000};return S_OK;}HRESULT ConfigureVerifiedFinalizedWrite()override{return S_OK;}HRESULT Write(IStream*,Storage::Job&)override{++writes;return S_OK;}};
  auto burner=std::make_shared<Burner>(device24);auto plan=std::make_shared<CdBurnPlan>();auto prepare=PrepareCdBurn(output,L"fake",plan,burner);reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Read}});blocked=run(prepare);Check(blocked.error==ERROR_BUSY&&!burner->opens,"burn preparation opened a busy recorder");reservation.reset();Check(!run(prepare).code,"independent burn preparation failed");auto burn=MakeCdBurnJob(*plan,burner);reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Read}});blocked=run(burn);Check(blocked.error==ERROR_BUSY&&!burner->writes,"burn ignored a reservation acquired after its dialog");reservation.reset();burner->key=device25;Check(run(burn).error==ERROR_MEDIA_CHANGED&&!burner->writes,"recorder alias change was not revalidated");burner->key=device24;Check(!run(burn).code&&burner->writes==1&&BackgroundCopies::Active(),"unrelated native burning remained globally blocked");
  auto busySourcePlan=std::make_shared<CdBurnPlan>();unsigned before=burner->opens;blocked=run(PrepareCdBurn(std::string((LPCSTR)(host+"\\busy-a.dsk")),L"fake",busySourcePlan,burner));Check(blocked.error==ERROR_BUSY&&burner->opens==before,"burn preparation read a busy source image");
  bool formatted=false;auto busyImageKey=a->ResourceKey();blocked=run(CommandStorage::Guard(CommandStorage::ImageWrite(VolumeCoord::Wide(std::string((LPCSTR)(host+"\\busy-a.dsk")))),[&](Storage::Job&){formatted=true;return Storage::Result{};}));Check(blocked.error==ERROR_BUSY&&!formatted,"format reservation ignored busy source image");
  CString newImage=host+"\\new.dsk";auto format=CommandStorage::Guard(CommandStorage::ImageWrite(VolumeCoord::Wide((LPCSTR)newImage)),[&](Storage::Job&){auto image=HfsPlus::CreateImage(VolumeCoord::Wide((LPCSTR)newImage).c_str(),8*1024*1024);HfsPlus::Volume::Format(*image,u"Independent",true);formatted=true;return Storage::Result{};});Check(!run(format).code&&formatted,"unrelated image formatting failed");
  fprintf(report,"PASS: burning reserves source image plus recorder/drive aliases, rejects late conflicts and remapped targets before recording, and formatting distinguishes busy images from unrelated new images.\n");
#ifdef HFV_FLOPPY_TESTING
  std::vector<BYTE> bytes;std::string why;Check(PrepareFloppyHfs("PolicyFloppy",bytes,why),"prepare policy floppy");TestFloppy disk(bytes);auto write=MakeFloppyWriteJob(24,bytes,&disk);reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Read}});blocked=run(write);Check(blocked.error==ERROR_BUSY&&!disk.disk.opens&&!disk.writes,"floppy format touched busy device");Check(!run(MakeFloppyWriteJob(25,bytes,&disk)).code&&disk.writes&&!disk.locked,"unrelated floppy write/verify failed");reservation.reset();
  const auto writesBefore=disk.writes;
  const auto confirmed=disk.disk.changeCount;
  ++disk.disk.changeCount;
  auto rediscovery=std::make_shared<std::atomic<unsigned>>(0);
  auto probe=[rediscovery](int drive){
   Check(drive==25,"media completion probed an unrelated drive");
   ++*rediscovery;
   return UINT(DRIVE_RAMDISK);
  };
  session.MacBrowser().SetDriveTypeForTest(probe);
  second.MacBrowser().SetDriveTypeForTest(probe);
  unsigned launches=0;
  auto confirmedWrite=[&](CommandStorage::Reservation held){
   ++launches;
   return MakeFloppyWriteJob(25,bytes,&disk,std::move(held),confirmed);
  };
  reservation=std::make_unique<VolumeCoord::Lease>(
      std::vector<VolumeCoord::Access>{{device25,VolumeCoord::Mode::Read}});
  Check(MediaCommands::Start(*frame,25,"Write test","Write",confirmedWrite).error==ERROR_BUSY&&
        launches==0&&rediscovery->load()==0,"busy command invalidated the drive or constructed a worker");
  reservation.reset();
  Check(!MediaCommands::Start(*frame,25,"Write test","Write",confirmedWrite).code,"queue coordinated floppy write");
  Until([&]{return rediscovery->load()==2&&!session.MacBrowser().MediaUpdatesPending()&&
        !second.MacBrowser().MediaUpdatesPending();});
  const auto firstRecovery=session.Mounts().FindWindowsDrive(25);
  const auto secondRecovery=second.Mounts().FindWindowsDrive(25);
  Check(BackgroundCopies::LastResult().error==ERROR_MEDIA_CHANGED&&launches==1&&
        disk.writes==writesBefore&&!disk.locked&&firstRecovery&&session.MountRoot(firstRecovery->address)&&
        secondRecovery&&second.MountRoot(secondRecovery->address)&&
        VolumeCoord::Available({{device25,VolumeCoord::Mode::Write}}),
        "floppy swap reached the writer or prevented browser recovery");
  session.MacBrowser().SetDriveTypeForTest({});
  second.MacBrowser().SetDriveTypeForTest({});
  fprintf(report,"PASS: media lifecycle rejects a busy command, detects a disk swapped after confirmation, and reconciles the drive in both browsers after failure.\n");
  fprintf(report,"PASS: simulated floppy format/write checks reader reservations before opening the device; an unrelated floppy completes write/readback and unlocks during the stalled copy.\n");
#endif
  CString cdFixture=host+"\\idle-cd.dsk";{auto image=HfsPlus::CreateImage(VolumeCoord::Wide((LPCSTR)cdFixture).c_str(),8*1024*1024);HfsPlus::Volume::Format(*image,u"IdleCD",true);}
  auto cdOne=MacBrowser::FromHfsPlus(HfsPlus::OpenImage(VolumeCoord::Wide((LPCSTR)cdFixture).c_str(),false),false,device25),cdTwo=MacBrowser::FromHfsPlus(HfsPlus::OpenImage(VolumeCoord::Wide((LPCSTR)cdFixture).c_str(),false),false,device25);int cdFirst=session.MacBrowser().AttachTestVolume(cdOne,"cd-first",25),cdSecond=second.MacBrowser().AttachTestVolume(cdTwo,"cd-second",25);Check(session.MacBrowser().ShowTestFolder(cdFirst,2)&&second.MacBrowser().ShowTestFolder(cdSecond,2)&&session.MacBrowser().ShowTestFolder(idle,2),"show idle CD fixtures");frame->GetBrowserSession().MacBrowser().ReleaseCd(25);other->GetBrowserSession().MacBrowser().ReleaseCd(25);Check(session.MacBrowser().Active()&&session.MacBrowser().SelectedResourceKey()==c->ResourceKey(),"releasing CD cleared an unrelated browser");bool closedOne=false,closedTwo=false;try{cdOne->Stat(2);}catch(const std::exception&){closedOne=true;}try{cdTwo->Stat(2);}catch(const std::exception&){closedTwo=true;}Check(closedOne&&closedTwo,"idle clipboard/backend references retained usable ejected media");HANDLE reopened=CreateFileA(cdFixture,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);Check(reopened!=INVALID_HANDLE_VALUE,"idle CD handles were retained after release");CloseHandle(reopened);
  fprintf(report,"PASS: releasing idle CD readers in two windows closes media handles even with retained backend references, while preserving an unrelated browser listing.\n");
  reservation=std::make_unique<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{device24,VolumeCoord::Mode::Write}});std::vector<int> probed;auto found=std::make_shared<std::vector<PhysicalVolume>>();Check(!run(MakeMediaScanJob(false,true,found,[&](int drive,bool){probed.push_back(drive);return drive==25;})).code&&std::find(probed.begin(),probed.end(),24)==probed.end()&&std::find(probed.begin(),probed.end(),25)!=probed.end()&&found->size()==1&&found->front().drive==25,"media scan globally stopped or probed busy drive");reservation.reset();
  // Opening another classic image extends this browser without closing any mounts.
  CString classic=host+"\\classic.dsk";{CFile file;Check(file.Open(classic,CFile::modeCreate|CFile::modeWrite),"create incremental image");file.SetLength(4*1024*1024);}char label[]="Incremental";Check(hfs_format(const_cast<char*>((LPCSTR)classic),0,label)==0,"format incremental image");HTREEITEM original=session.Tree()->m_ct->GetRootItem();Check(doc->OnOpenDocument(classic)&&IsWindow(frame->GetSafeHwnd())&&session.Tree()->m_ct->GetItemData(original)!=0&&BackgroundCopies::Active(),"classic image opening rebuilt/closed unrelated mounts");Check(session.MacBrowser().ShowTestFolder(idle,2),"idle image lost after classic open");Check(doc->OnOpenDocument(newImage),"open newly formatted native image");
  Check(doc->OnOpenDocument(host+"\\busy-a.dsk")&&BackgroundCopies::Active(),"busy image selection did not defer");SetEvent(stalled.release);Until([]{return !BackgroundCopies::Active();});Until([&]{return session.MacBrowser().Active()&&Row(session,L"Slow")>=0;});Check(Find(*b,u"Slow")&&FileOperations::Pending().empty()&&VolumeCoord::Available({{a->ResourceKey(),VolumeCoord::Mode::Write},{b->ResourceKey(),VolumeCoord::Mode::Write}}),"policy changes corrupted copy or leaked reservations");
  fprintf(report,"PASS: scans skip only busy drives; opening unrelated classic/native images preserves existing roots and a busy image opens after its job completes. Original copy data and cleanup remain intact. No real hardware or computer control used.\n");

  struct PendingRead {
   std::shared_ptr<Gate> gate=std::make_shared<Gate>();
   PendingRead(std::wstring target,DWORD uiThread){
    VolumeCoord::SetPathLookupObserver([gate=gate,target=std::move(target),uiThread](const auto& path){
     if(path!=target||WaitForSingleObject(gate->entered,0)==WAIT_OBJECT_0)return;
     Check(GetCurrentThreadId()!=uiThread,"background command bound a path on the UI thread");
     SetEvent(gate->entered);
     Check(WaitForSingleObject(gate->release,15000)==WAIT_OBJECT_0,"command path gate timed out");
    });
   }
   ~PendingRead(){SetEvent(gate->release);VolumeCoord::SetPathLookupObserver({});}
   void Entered(){Until([&]{return WaitForSingleObject(gate->entered,0)==WAIT_OBJECT_0;});}
   void Release(){SetEvent(gate->release);}
  };
  const auto uiThread=GetCurrentThreadId();
  const std::string formatPath=std::string(host.GetString())+"\\background-format.dsk";
  {
   PendingRead format(VolumeCoord::Wide(formatPath),uiThread);
   const auto enteredPath=std::string(host.GetString())+"\\.\\background-format.dsk";
   Check(StartClassicFormat(session,{enteredPath,"Background HFS",2*1024*1024}),"start classic image format");
   format.Entered();
   Host(second,host);
   Check(frame->IsWindowEnabled()&&other->IsWindowEnabled()&&second.ListingAvailable()&&!StorageUi::busy(),
         "formatting blocked an unrelated browser");
   format.Release();
   Until([&]{return !BackgroundCopies::Active()&&!session.Navigating();});
   Check(!BackgroundCopies::LastResult().code&&session.WindowTitle().find(L"Background HFS")!=std::wstring::npos&&
         second.MacBrowser().TargetFolder().IsWindows(),"formatted image did not open in its originating browser");
  }
  Check(StartClassicFormat(session,{formatPath,"Unwanted",4*1024*1024}),"submit existing image as a format target");
  Until([]{return !BackgroundCopies::Active();});
  Check(BackgroundCopies::LastResult().code&&session.WindowTitle().find(L"Background HFS")!=std::wstring::npos,
        "background format replaced an existing image");

  const auto folder=std::string(host.GetString())+"\\find-background";
  Check(CreateDirectoryA(folder.c_str(),nullptr),"create background search scope");
  {CFile file;Check(file.Open((folder+"\\Needle.txt").c_str(),CFile::modeCreate|CFile::modeWrite),"create search match");}
  FindRequest search;
  search.scope.origin=FileOperations::Location::Windows(VolumeCoord::Wide(folder));
  search.pattern="Needle";
  unsigned results=0;
  auto receive=[&](BrowserSession& owner,std::shared_ptr<const FindResults> found,const Storage::Result& result){
   Check(&owner==&session&&!result.code&&found->hits.size()==1,"search result went to a different browser or folder");
   ++results;
  };
  {
   PendingRead read(VolumeCoord::Wide(folder),uiThread);
   Check(StartFindSearch(session,search,receive),"start background Find Files");
   read.Entered();
   Host(second,host);
   Check(BackgroundCopies::Active()&&other->IsWindowEnabled()&&second.ListingAvailable()&&results==0,
         "slow Find Files blocked browsing elsewhere");
   read.Release();
   Until([&]{return !BackgroundCopies::Active();});
   Check(results==1,"the originating browser did not receive the completed search");
  }
  {
   PendingRead read(VolumeCoord::Wide(folder),uiThread);
   Check(StartFindSearch(session,search,receive),"start search before closing its browser");
   read.Entered();
   const auto closing=frame->GetSafeHwnd();
   frame->SendMessage(WM_CLOSE);
   Check(!IsWindow(closing)&&other->IsWindowEnabled(),"active search prevented independent window closure");
   read.Release();
   Until([]{return !BackgroundCopies::Active();});
   Check(results==1&&second.ListingAvailable(),"closed browser search published to another window");
  }
  fprintf(report,"PASS: classic format and Find Files run without blocking another browser; results go to their origin and are suppressed after it closes.\n");
  {
   const auto newImage=VolumeCoord::Wide(std::string(host.GetString())+"\\closed-origin.dsk");
   auto* origin=app.CreateBrowser(FALSE);
   Check(origin,"open a separate formatter browser");
   PendingRead format(newImage,uiThread);
   const auto closing=origin->GetSafeHwnd();
   Check(StartMacImageFormat(origin->GetBrowserSession(),{newImage,{},u"Detached format",8*1024*1024}),
         "start HFS+ format in a window that will close");
   format.Entered();
   Host(second,host);
   Until([&]{return !second.Navigating()&&second.ListingAvailable();});
   Check(other->IsWindowEnabled(),"background HFS+ format blocked the surviving browser");
   origin->SendMessage(WM_CLOSE);
   Check(!IsWindow(closing),"format prevented the originating browser from closing");
   format.Release();
   Until([&]{return !BackgroundCopies::Active()&&!second.Navigating()&&second.ListingAvailable();});
   const auto& mounts=second.Mounts().Mounted();
   Check(!BackgroundCopies::LastResult().code&&GetFileAttributesW(newImage.c_str())!=INVALID_FILE_ATTRIBUTES&&
         second.MacBrowser().TargetFolder().IsWindows()&&
         std::none_of(mounts.begin(),mounts.end(),[&](const auto& mount){return mount.key==newImage;}),
         "format was lost or opened in a different browser after the origin closed");
   fprintf(report,"PASS: HFS+ image formatting finishes after its originating window closes and leaves the surviving browser alone.\n");
  }
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last copy: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 SetEvent(stalled.release);BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunWindowsPathCoordinationTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\windows-path-coordination-report.txt","w");if(!report)return 2;int result=0;Gate gate,planningGate;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 std::vector<FileOperations::Operation> queued;
 try{
  auto wide=[](const CString& path){return VolumeCoord::Wide((LPCSTR)path);};
  auto save=[&](const CString& path,const char* text){CFile file;Check(file.Open(path,CFile::modeCreate|CFile::modeWrite),"create path fixture");file.Write(text,UINT(strlen(text)));};
  auto exists=[](const CString& path){return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;};
  CString a=host+"\\A",b=host+"\\B",input=host+"\\Input";for(const auto& path:{a,b,input})Check(CreateDirectoryA(path,nullptr),"create path directories");save(a+"\\One","ONE");save(a+"\\Two","TWO");save(b+"\\Other","OTHER");save(input+"\\Held","HELD DATA");save(input+"\\Fail","FAIL DATA");
  auto aKey=VolumeCoord::PathKey(wide(a)),bKey=VolumeCoord::PathKey(wide(b)),oneKey=VolumeCoord::PathKey(wide(a+"\\One")),twoKey=VolumeCoord::PathKey(wide(a+"\\Two"));
  {VolumeCoord::Lease held({{oneKey,VolumeCoord::Mode::Read}});Check(VolumeCoord::Available({{twoKey,VolumeCoord::Mode::Write}})&&VolumeCoord::Available({{bKey,VolumeCoord::Mode::Write}})&&!VolumeCoord::Available({{aKey,VolumeCoord::Mode::Write}}),"siblings or ancestor conflict incorrect");bool rejected=false;try{VolumeCoord::Lease all({{bKey,VolumeCoord::Mode::Write},{oneKey,VolumeCoord::Mode::Write}});}catch(const VolumeCoord::Busy&){rejected=true;}Check(rejected&&VolumeCoord::Available({{bKey,VolumeCoord::Mode::Write}}),"failed atomic acquisition retained a partial reservation");}
  {VolumeCoord::Lease writer({{oneKey,VolumeCoord::Mode::Write}});Check(VolumeCoord::Available({{aKey,VolumeCoord::Mode::Browse}})&&!VolumeCoord::Available({{aKey,VolumeCoord::Mode::Read}}),"directory metadata sharing differs from recursive source reads");VolumeCoord::Lease other({{twoKey,VolumeCoord::Mode::Read}});bool rejected=false;try{VolumeCoord::OperationLease expanded({{aKey,VolumeCoord::Mode::Write}});}catch(const VolumeCoord::Busy&){rejected=true;}Check(rejected&&!VolumeCoord::Readable(oneKey),"nested reservation masked a sibling reader or released its outer writer");}
  auto missing=VolumeCoord::PathKey(wide(a+"\\Created"));{VolumeCoord::Lease held({{missing,VolumeCoord::Mode::Write}});save(a+"\\Created","NEW");Check(!VolumeCoord::Readable(VolumeCoord::PathKey(wide(a+"\\Created"))),"publication changed a reserved destination's identity");}
  Check(CreateHardLinkA(b+"\\Alias",a+"\\One",nullptr),"create hard-link alias");CString junction=host+"\\Junction";Check(Junction(junction,a),"create junction alias");
  {VolumeCoord::Lease held({{VolumeCoord::PathKey(wide(a+"\\One")),VolumeCoord::Mode::Read}});for(const auto& alias:{b+"\\Alias",junction+"\\One",a+"\\..\\A\\One",a+"\\One:alternate"})Check(!VolumeCoord::Available({{VolumeCoord::PathKey(wide(alias)),VolumeCoord::Mode::Write}}),"file alias bypassed a reader");Check(!VolumeCoord::Available({{VolumeCoord::PathKey(wide(junction)),VolumeCoord::Mode::Write}}),"junction parent bypassed a child reader");}
  {VolumeCoord::Lease held({{aKey,VolumeCoord::Mode::Read}});Check(!VolumeCoord::Available({{VolumeCoord::PathKey(wide(b+"\\Alias")),VolumeCoord::Mode::Write}}),"outside hard link bypassed a recursive source reservation");}
  {VolumeCoord::Lease held({{VolumeCoord::PathKey(wide(a+"\\NewLeaf")),VolumeCoord::Mode::Write}});Check(!VolumeCoord::Readable(VolumeCoord::PathKey(wide(junction+"\\newleaf"))),"case/junction alias bypassed a missing destination");}
  wchar_t shortName[32768]{};DWORD shortLength=GetShortPathNameW(wide(a+"\\One").c_str(),shortName,ARRAYSIZE(shortName));if(shortLength&&shortLength<ARRAYSIZE(shortName)){VolumeCoord::Lease held({{oneKey,VolumeCoord::Mode::Write}});Check(!VolumeCoord::Readable(VolumeCoord::PathKey(shortName)),"short-name alias bypassed a writer");}
  CString moved=host+"\\Moved",renamed=host+"\\Renamed";Check(CreateDirectoryA(moved,nullptr),"create movable parent");save(moved+"\\Child","CHILD");{VolumeCoord::Lease held({{VolumeCoord::PathKey(wide(moved+"\\Child")),VolumeCoord::Mode::Read}});Check(MoveFileA(moved,renamed),"simulate external parent rename");Check(!VolumeCoord::Available({{VolumeCoord::PathKey(wide(renamed)),VolumeCoord::Mode::Write}}),"renamed parent identity bypassed a child reservation");}
  VolumeCoord::Subscription watchA,watchB,watchParent;watchA.Watch(aKey);watchB.Watch(bKey);watchParent.Watch(VolumeCoord::PathKey(wide(host)));VolumeCoord::Changed({VolumeCoord::PathKey(wide(a+"\\Two"))});Check(!watchA.Drain().empty()&&watchB.Drain().empty()&&!watchParent.Drain().empty(),"notifications refreshed an unrelated sibling or missed an ancestor");
  fprintf(report,"PASS: sibling file/folder independence; recursive parent/child exclusion; atomic all-or-nothing acquisition; nested reservation safety; destination publication identity; hard-link, junction, case, dot, alternate-stream and short-path aliases; renamed-parent identity; targeted parent notifications.\n");
  // Image internals remain exclusive against operations on the image file itself.
  CString imagePath=a+"\\Disk.dsk";{auto image=HfsPlus::CreateImage(wide(imagePath).c_str(),8*1024*1024);HfsPlus::Volume::Format(*image,u"Image",true);}auto imageKey=VolumeCoord::ImageKey(wide(imagePath));{VolumeCoord::Lease held({{imageKey,VolumeCoord::Mode::Write}});Check(!VolumeCoord::Readable(VolumeCoord::PathKey(wide(imagePath)))&&!VolumeCoord::Available({{aKey,VolumeCoord::Mode::Write}})&&VolumeCoord::Readable(bKey),"image-file and folder reservations are inconsistent");}
  fprintf(report,"PASS: mounted-image internal writes conflict with raw access/deletion of their image file or parent directory, while unrelated Windows folders remain available.\n");
  struct GatedWindows:MacBrowser::Volume {
   std::shared_ptr<MacBrowser::Volume> base;Gate& gate;std::atomic<bool> fail{false};
   GatedWindows(std::shared_ptr<MacBrowser::Volume> value,Gate& g):base(value),gate(g){}
   std::wstring HostPath(uint32_t id)const override{return base->HostPath(id);}std::string ResourceKey()const override{return base->ResourceKey();}std::u16string Name()const override{return base->Name();}const char* Kind()const override{return base->Kind();}bool Writable()const override{return base->Writable();}bool CaseSensitive()const override{return false;}uint64_t FreeBytes()const override{return base->FreeBytes();}void Close()override{base->Close();}
   MacBrowser::Entry Stat(uint32_t id)override{return base->Stat(id);}std::vector<MacBrowser::Entry> List(uint32_t id)override{return base->List(id);}void Read(uint32_t id,unsigned fork,uint64_t at,void* p,size_t n)override{if(n){SetEvent(gate.entered);Check(WaitForSingleObject(gate.release,15000)==WAIT_OBJECT_0,"Windows read gate not released");if(fail)throw HfsPlus::Error("Injected Windows source read failure");}base->Read(id,fork,at,p,n);base->Close();}
   uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override{return base->MakeDirectory(parent,name);}uint32_t Put(uint32_t parent,const MacBrowser::Entry& e,MacBrowser::Reader r,bool replace)override{return base->Put(parent,e,r,replace);}void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{base->Rename(id,parent,name);}void Remove(uint32_t id)override{base->Remove(id);}
  };
  const auto companionName=L"Read Me \u65e5";
  {
   auto folder=MacBrowser::OpenWindows(wide(b));
   MacBrowser::Entry entry;entry.name=MacBrowser::U16(companionName);entry.forks[0].size=11;entry.forks[1].size=19;
   memcpy(entry.finder.data(),"TEXTttxt",8);
   folder->Put(2,entry,[](unsigned fork,uint64_t,void* output,size_t bytes){memset(output,fork?'R':'D',bytes);},false);
  }
  save(b+"\\Legacy","Finder-only data");
  {
   // A version 1 AppleDouble file with a 16-byte classic Finder record.
   uint8_t header[54]{};
   HfsPlus::Disk::P32(header,0x00051607);
   HfsPlus::Disk::P32(header+4,0x00010000);
   HfsPlus::Disk::P16(header+24,1);
   HfsPlus::Disk::P32(header+26,9);
   HfsPlus::Disk::P32(header+30,38);
   HfsPlus::Disk::P32(header+34,16);
   memcpy(header+38,"TEXTttxt",8);
   CFile sidecar;
   Check(sidecar.Open(b+"\\%Legacy",CFile::modeCreate|CFile::modeWrite),"create legacy companion");
   sidecar.Write(header,sizeof header);
  }
  save(b+"\\%Orphan","Unpaired percent file");
  save(b+"\\%Other","Invalid companion");
  auto listing=[&](BrowserSession& session){
   std::map<std::u16string,BrowserDirectory::Row> rows;
   auto& control=session.List()->GetListCtrl();
   for(int i=0;i<control.GetItemCount();++i)if(const auto row=session.List()->BrowserRowAt(i))
    rows.emplace(row->entry.name,*row);
   return rows;
  };
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto first=app.CreateBrowser(FALSE),second=app.CreateBrowser(FALSE);Check(first&&second,"create independent Windows browsers");auto& left=first->GetBrowserSession();auto& right=second->GetBrowserSession();Host(left,a);Host(right,b);
  const auto idle=listing(right);
  const auto unicode=idle.find(MacBrowser::U16(companionName));
  Check(unicode!=idle.end()&&unicode->second.location.IsWindows()&&
        unicode->second.location.WindowsPath()==wide(b)+L"\\"+companionName&&
        unicode->second.entry.forks[1].size==19&&
        !memcmp(unicode->second.entry.finder.data(),"TEXTttxt",8)&&
        idle.count(u"Other")==1&&idle.count(u"%Orphan")==1&&idle.count(u"%Other")==1&&idle.size()==6,
        "idle Windows listing lost Unicode, Finder metadata, or an ordinary percent-prefixed file");
  const auto legacy=idle.find(u"Legacy");
  Check(legacy!=idle.end()&&!memcmp(legacy->second.entry.finder.data(),"TEXTttxt",8)&&
        legacy->second.entry.forks[1].size==0&&idle.count(u"%Legacy")==0,
        "legacy Finder-only companion was not merged with its data file");
  auto resolved=MacBrowser::ResolveWindowsFile(wide(input+"\\Held"));auto slow=std::make_shared<GatedWindows>(resolved.first,gate);FileOperations::Request request;FileOperations::Location source,target;source.SetMounted(slow,resolved.second);source.expectedName=u"Held";target.SetWindows(wide(a));target.expectedIdentity=MacBrowser::DescribeWindowsFile(wide(a)).identity;request.Copy().sources={source};request.Copy().destination=target;
  auto slowPath=wide(input+"\\Held");DWORD uiThread=GetCurrentThreadId();std::atomic<unsigned> plans{0};VolumeCoord::SetPathLookupObserver([&](const std::wstring& path){if(path.size()>=slowPath.size()&&!_wcsicmp(path.c_str()+path.size()-slowPath.size(),slowPath.c_str())){Check(GetCurrentThreadId()!=uiThread,"Windows path planning ran on the UI thread");if(++plans==1){SetEvent(planningGate.entered);Check(WaitForSingleObject(planningGate.release,15000)==WAIT_OBJECT_0,"Windows planner not released");}}});
  Check(BackgroundCopies::Start({request},first,true),"start Windows copy");Until([&]{return WaitForSingleObject(planningGate.entered,0)==WAIT_OBJECT_0;});Check(first->IsWindowEnabled()&&second->IsWindowEnabled()&&right.List()->StorageSelectionAvailable(),"slow path planning blocked UI");Host(right,b);SetEvent(planningGate.release);Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});Check(first->IsWindowEnabled()&&second->IsWindowEnabled(),"Windows copy disabled browsers");
  Host(right,b);
  const auto active=listing(right);
  Check(active.size()==idle.size()&&BackgroundCopies::Active(),"paused copy changed the sibling folder listing");
  for(const auto& saved:idle){
   const auto found=active.find(saved.first);
   Check(found!=active.end(),"paused copy changed a displayed Windows filename");
   const auto& before=saved.second;const auto& during=found->second;
   Check(before.location.WindowsPath()==during.location.WindowsPath()&&
         before.entry.identity==during.entry.identity&&before.accessKey==during.accessKey&&
         before.entry.finder==during.entry.finder&&
         before.entry.forks[0].size==during.entry.forks[0].size&&before.entry.forks[1].size==during.entry.forks[1].size,
         "paused copy changed typed paths or AppleDouble metadata");
  }
  fprintf(report,"PASS: Windows names, typed Unicode paths, Finder codes, and fork sizes agree before and during a paused copy.\n");
  // Actual staging entries are protected until publication/rollback finishes.
  WIN32_FIND_DATAA temporary{};HANDLE find=FindFirstFileA(a+"\\HFV*.tmp",&temporary);Check(find!=INVALID_HANDLE_VALUE,"copy did not stage output");CString staged=a+"\\"+temporary.cFileName;FindClose(find);Check(!VolumeCoord::Readable(VolumeCoord::PathKey(wide(staged))),"copy staging file was not reserved");
  // An independent copy may share the read-only source, and its target is on the same drive.
  FileOperations::Request independent;FileOperations::Location ordinary;ordinary.SetWindows(wide(input+"\\Held"));independent.Copy().sources={ordinary};independent.Copy().destination.SetWindows(wide(b));independent.background=true;auto parallel=FileOperations::Submit(independent);Until([&]{return parallel.Job()->done();});Check(!parallel.Job()->result().code&&BackgroundCopies::Active()&&exists(b+"\\Held"),"same-drive independent Windows copy serialized");Until([&]{return Row(right,L"Held")>=0;});
  auto heldProgress=BackgroundCopies::DialogWindow();unsigned before=right.notificationRefreshes;Select(left,L"Two");int row=Row(left,L"Two");Check(left.List()->StorageSelectionAvailable()&&left.List()->TestCommitLabel(row,"RenamedTwo")==0,"sibling rename could not submit");Until([&]{return exists(a+"\\RenamedTwo")&&Row(left,L"RenamedTwo")>=0;});Check(BackgroundCopies::Active()&&right.notificationRefreshes==before,"sibling rename waited for copy or refreshed unrelated browser");
  Select(left,L"Created");left.List()->TestDeleteSelection();Until([&]{return !exists(a+"\\Created")&&Row(left,L"Created")<0;});Check(BackgroundCopies::Active()&&right.notificationRefreshes==before,"sibling delete waited for copy or refreshed unrelated folder");
  auto parent=left.MacBrowser().TargetFolder(); // Test helper navigation's tree root is the fixture host; use explicit A below.
  parent.SetWindows(wide(a));parent.expectedIdentity=MacBrowser::DescribeWindowsFile(wide(a)).identity;Check(left.MacBrowser().CreateFolder(parent,u"NewSibling"),"start sibling folder creation");Until([&]{return exists(a+"\\NewSibling")&&Row(left,L"NewSibling")>=0;});Check(BackgroundCopies::Active(),"sibling folder creation serialized");
  // Conflicting source, destination, companion and parent operations queue without
  // consuming every worker; cancellation must not apply any of them.
  auto remove=[&](const CString& path,bool directory){FileOperations::Location p;p.SetWindows(wide(path));p.directory=directory;FileOperations::Request r=FileOperations::DeleteEntries{{p}};r.background=true;return FileOperations::Submit(r);};
  queued.push_back(remove(a+"\\Held",false));queued.push_back(remove(a+"\\%Held",false));queued.push_back(remove(a,true));queued.push_back(remove(input+"\\Held",false));queued.push_back(remove(staged,false));for(auto& op:queued)Check(!op.Job()->done(),"conflicting Windows operation ran during copy");
  FileOperations::Request finalIndependent;finalIndependent = FileOperations::CreateFolder{};finalIndependent.background=true;finalIndependent.Get<FileOperations::CreateFolder>()->parent.SetWindows(wide(b));finalIndependent.Get<FileOperations::CreateFolder>()->name=u"QueueStillRuns";auto skippedQueue=FileOperations::Submit(finalIndependent);Until([&]{return skippedQueue.Job()->done();});Check(!skippedQueue.Job()->result().code&&BackgroundCopies::Active(),"conflicts starved unrelated Windows work");
  for(auto& op:queued)op.Job()->cancel();for(auto& op:queued)Until([&]{return op.Job()->done();});SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});VolumeCoord::SetPathLookupObserver({});Check(!BackgroundCopies::LastResult().code&&exists(a+"\\Held")&&exists(a+"\\%Held")&&exists(input+"\\Held"),"copy or canceled conflicts lost data");Until([&]{return Row(left,L"Held")>=0;});Check(!IsWindow(heldProgress)&&FileOperations::Pending().empty()&&VolumeCoord::Available({{VolumeCoord::PathKey(wide(a)),VolumeCoord::Mode::Write},{VolumeCoord::PathKey(wide(input)),VolumeCoord::Mode::Write}}),"Windows completion leaked locks/progress");
  {
   const auto sourcePath=wide(input+"\\Held");
   const auto movedPath=wide(input+"\\StaleRename");
   const auto replacementPath=wide(input+"\\Replacement");
   auto location=FileOperations::Location::Windows(sourcePath);
   location.expectedIdentity=MacBrowser::DescribeWindowsFile(sourcePath).identity;
   location.requiresWindowsIdentity=true;
   Check(!location.expectedIdentity.empty(),"capture Windows rename identity");
   const auto key=VolumeCoord::PathKey(sourcePath);
   const auto destination=VolumeCoord::PathKey(movedPath);
   std::optional<VolumeCoord::Lease> occupied;
   occupied.emplace(std::vector<VolumeCoord::Access>{{key,VolumeCoord::Mode::Write}});
   auto lookups=std::make_shared<std::atomic<unsigned>>(0);
   VolumeCoord::SetPathLookupObserver([movedPath,lookups](const std::wstring& path){
    if(path==movedPath)++*lookups;
   });
   FileOperations::Request rename=FileOperations::RenameEntry{location,u"StaleRename"};
   rename.background=true;
   auto cancelled=FileOperations::Submit(rename);
   queued.push_back(cancelled);
   Until([&]{return lookups->load()==1;});
   Check(!cancelled.Job()->done(),"prepared rename bypassed its reservation");
   cancelled.Job()->cancel();
   Until([&]{return cancelled.Job()->done();});
   Check(cancelled.Job()->result().error==ERROR_CANCELLED&&lookups->load()==1&&
         exists(input+"\\Held")&&!exists(input+"\\StaleRename"),
         "cancelled prepared rename entered execution");

   auto stale=FileOperations::Submit(rename);
   queued.push_back(stale);
   Until([&]{return lookups->load()==2;});
   Check(!stale.Job()->done(),"second prepared rename bypassed its reservation");
   {CFile replacement;Check(replacement.Open(input+"\\Replacement",CFile::modeCreate|CFile::modeWrite),"create Windows replacement");
    replacement.Write("new",3);}
   Check(MoveFileExW(replacementPath.c_str(),sourcePath.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE,
         "replace reserved Windows selection externally");
   occupied.reset();
   Until([&]{return stale.Job()->done();});
   VolumeCoord::SetPathLookupObserver({});
   Check(stale.Job()->result().code&&lookups->load()>=3&&
         MacBrowser::DescribeWindowsFile(sourcePath).forks[0].size==3&&
         !exists(input+"\\StaleRename")&&
         VolumeCoord::Available({{key,VolumeCoord::Mode::Write},{destination,VolumeCoord::Mode::Write}}),
         "path rebinding accepted a stale selection or retained its reservation");
   fprintf(report,"PASS: the same queued job can cancel before execution; a replaced Windows selection fails after rebinding and releases its leases.\n");
  }
  fprintf(report,"PASS: real same-drive background copies share a reader and finish independently; sibling rename/delete/new-folder jobs finish and refresh during a stalled copy; unrelated browser refresh stays unchanged; source/destination/resource-companion/staging/parent conflicts queue and cancel without starving unrelated work.\n");
  for(bool failure:{true,false}){gate.Reset();auto failSource=MacBrowser::ResolveWindowsFile(wide(input+"\\Fail"));auto stalled=std::make_shared<GatedWindows>(failSource.first,gate);stalled->fail=failure;request.Copy().sources.front().SetMounted(stalled,failSource.second);request.Copy().sources.front().expectedName=u"Fail";request.Copy().destination.expectedIdentity=MacBrowser::DescribeWindowsFile(wide(a)).identity;Check(BackgroundCopies::Start({request},first,true),"start failed/canceled Windows copy");Until([&]{return WaitForSingleObject(gate.entered,0)==WAIT_OBJECT_0;});auto progress=BackgroundCopies::DialogWindow();if(!failure)SendMessage(progress,WM_COMMAND,IDCANCEL,0);Host(left,b);SetEvent(gate.release);Until([]{return !BackgroundCopies::Active();});Check(BackgroundCopies::LastResult().code&&(!failure?BackgroundCopies::LastResult().error==ERROR_CANCELLED:true)&&!exists(a+"\\Fail")&&!exists(a+"\\%Fail")&&exists(input+"\\Fail")&&first->IsWindowEnabled(),"failed/canceled Windows copy published output or lost source");if(IsWindow(progress))SendMessage(progress,WM_COMMAND,IDCANCEL,0);Check(VolumeCoord::Available({{VolumeCoord::PathKey(wide(a)),VolumeCoord::Mode::Write},{VolumeCoord::PathKey(wide(input)),VolumeCoord::Mode::Write}}),"failed Windows copy retained reservations");}
  find=FindFirstFileA(a+"\\HFV*.tmp",&temporary);Check(find==INVALID_HANDLE_VALUE,"Windows rollback left staging files");if(find!=INVALID_HANDLE_VALUE)FindClose(find);
  fprintf(report,"PASS: source-read failure and cancellation after a blocked final chunk remove partial Windows output/companions/staging, retain sources and release every reservation without blocking browsing. Images/fixtures only; no hardware or computer control.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last copy: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 SetEvent(planningGate.release);SetEvent(gate.release);for(auto& op:queued)op.Job()->cancel();BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});for(auto& op:queued)op.Job()->wait();VolumeCoord::SetPathLookupObserver({});ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunExtendedAttributesTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\extended-attributes-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create attributes browser");auto& session=frame->GetBrowserSession();auto native=Image(host+"\\source.dsk"),destination=Image(host+"\\destination.dsk");auto file=Put(*native,u"File");auto folder=native->MakeDirectory(2,u"Folder");
  HfsPlus::Bytes security(68,0);HfsPlus::Disk::P32(security.data(),0x012cc16d);HfsPlus::Disk::P32(security.data()+36,1);HfsPlus::Disk::P32(security.data()+40,0x20000);HfsPlus::Disk::P32(security.data()+60,0x61);HfsPlus::Disk::P32(security.data()+64,0x100083);for(unsigned i=0;i<16;++i)security[44+i]=uint8_t(i+1);
  HfsPlus::Attributes attrs{{u"user.Empty",{}},{u"user.Case",{1,0,2}},{u"user.case",HfsPlus::Bytes(8003,0x83)},{u"com.apple.system.Security",security}};native->SetAttributes(file,attrs);native->SetAttributes(folder,attrs);MacBrowser::Entry child;child.name=u"Child";child.forks[0].size=3;child.attributes=attrs;child.attributesKnown=true;auto childId=native->Put(folder,child,[](unsigned,uint64_t,void* p,size_t n){memset(p,'C',n);},false);
  CString windows=host+"\\windows";Check(CreateDirectoryA(windows,nullptr),"create attribute export folder");auto pc=MacBrowser::OpenWindows(VolumeCoord::Wide((LPCSTR)windows));
  auto copy=[&](MacBrowser::Volume& from,uint32_t id,MacBrowser::Volume& to,bool move=false){Storage::Job job;Check(MacBrowser::Copy(from,id,to,2,job,move),"copy attributes");};
  copy(*native,file,*destination);Check(destination->GetAttributes(Find(*destination,u"File"))==attrs,"Native copy lost attributes or ACL");copy(*native,file,*pc);pc->Close();auto exported=Find(*pc,u"File");Check(pc->GetAttributes(exported)==attrs,"AppleDouble export lost attributes/ACL");char bytes[3]{};pc->Read(exported,0,0,bytes,3);Check(BYTE(bytes[1])==1,"Attribute export changed data fork");pc->Close();pc->Rename(exported,2,u"Roundtrip");copy(*pc,exported,*destination);Check(destination->GetAttributes(Find(*destination,u"Roundtrip"))==attrs,"AppleDouble import lost attributes/ACL");
  copy(*native,folder,*pc);auto exportedFolder=Find(*pc,u"Folder");Check(pc->GetAttributes(exportedFolder)==attrs,"Folder companion lost ACL");pc->Close();copy(*pc,exportedFolder,*destination);auto nativeFolder=Find(*destination,u"Folder");Check(destination->GetAttributes(nativeFolder)==attrs,"Directory roundtrip lost attributes");auto children=destination->List(nativeFolder);Check(children.size()==1&&destination->GetAttributes(children.front().id)==attrs,"Nested file metadata lost");
  pc->Rename(exportedFolder,2,u"RenamedFolder");Check(pc->GetAttributes(exportedFolder)==attrs&&GetFileAttributesA(windows+"\\%RenamedFolder")!=INVALID_FILE_ATTRIBUTES,"Folder rename orphaned companion");Storage::Job deletion;MacBrowser::RemoveTree(*pc,exportedFolder,deletion);Check(GetFileAttributesA(windows+"\\%RenamedFolder")==INVALID_FILE_ATTRIBUTES,"Directory deletion orphaned metadata companion");
  // Refuse lossy legacy copies before producing any destination or deleting source.
  CString classic=host+"\\classic.dsk";{CFile f;Check(f.Open(classic,CFile::modeCreate|CFile::modeWrite),"create attribute rejection image");f.SetLength(1474560);}char label[]="NoAttributes";Check(hfs_format(const_cast<char*>((LPCSTR)classic),0,label)==0,"format attribute rejection image");auto legacy=MacBrowser::OpenClassic((LPCSTR)classic,"NoAttributes:",true);bool rejected=false;try{copy(*native,file,*legacy,true);}catch(const std::exception&){rejected=true;}Check(rejected&&legacy->List().empty()&&native->GetAttributes(file)==attrs,"Lossy legacy transfer wrote destination or removed source");legacy->Close();

  // A failed replacement must leave both the old companion and data intact.
  {auto e=pc->Stat(exported);e.attributes=attrs;e.attributesKnown=true;bool failed=false;try{pc->Put(2,e,[](unsigned,uint64_t,void*,size_t){throw HfsPlus::Error("Injected attributed copy failure");},true);}catch(const std::exception&){failed=true;}Check(failed&&pc->GetAttributes(exported)==attrs&&pc->Stat(exported).forks[0].size==8192,"Failed attributed replacement lost original data/metadata");}
  // Malformed ATTR offsets must fail before an import can replace anything.
  {pc->Close();CString companion=windows+"\\%Roundtrip";CFile f;Check(f.Open(companion,CFile::modeReadWrite),"open ATTR corruption fixture");BYTE saved[4],bad[4]{};f.Seek(96,CFile::begin);Check(f.Read(saved,4)==4,"read ATTR data-start");f.Seek(96,CFile::begin);f.Write(bad,4);f.Close();bool rejected=false;try{Storage::Job job;MacBrowser::Copy(*pc,exported,*destination,2,job);}catch(const std::exception&){rejected=true;}Check(rejected&&destination->GetAttributes(Find(*destination,u"Roundtrip"))==attrs,"Malformed ATTR replaced existing native metadata");Check(f.Open(companion,CFile::modeReadWrite),"restore ATTR corruption fixture");f.Seek(96,CFile::begin);f.Write(saved,4);f.Close();Check(pc->GetAttributes(exported)==attrs,"ATTR fixture restore failed");}
  // Values and aggregate metadata exceed the former application caps. The ATTR
  // directory also exceeds 64 KiB and includes a >127-byte UTF-8 name.
  {auto make=[&](const char* name){auto path=VolumeCoord::Wide(std::string((LPCSTR)host)+"\\"+name);{auto image=HfsPlus::CreateImage(path.c_str(),512ULL<<20);HfsPlus::Volume::Format(*image,u"Streamed",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);}return MacBrowser::OpenImage(path);};auto large=make("large-source.dsk"),back=make("large-target.dsk");
   auto pattern=[](uint64_t at,void* p,size_t n){memset(p,0,n);if(at==0&&n)static_cast<BYTE*>(p)[0]=0x83;};HfsPlus::Attributes values{{u"user.BigA",HfsPlus::AttributeData::Snapshot((70ULL<<20)+5,pattern)},{u"user.BigB",HfsPlus::AttributeData::Snapshot((70ULL<<20)+9,pattern)},{std::u16string(100,u'\u00e9'),{1,2,3}}};for(unsigned i=0;i<600;++i){auto name=std::u16string(116,u'x')+MacBrowser::U16(std::to_wstring(i));values[name]={uint8_t(i)};}
   MacBrowser::Entry metadata;metadata.name=u"Large metadata";metadata.attributesKnown=true;metadata.attributes=values;auto id=large->Put(2,metadata,[](unsigned,uint64_t,void*,size_t){},false);copy(*large,id,*pc);auto windowsId=Find(*pc,metadata.name);Check(pc->GetAttributes(windowsId)==values,"Large AppleDouble streaming export changed attributes");copy(*pc,windowsId,*back);Check(back->GetAttributes(Find(*back,metadata.name))==values,"Large AppleDouble streaming import changed attributes");
   HfsPlus::Attributes longName{{std::u16string(127,u'\u4e00'),{1}}};
   metadata.name=u"Long attribute name";metadata.attributes=longName;
   const auto longNameId=pc->Put(2,metadata,[](unsigned,uint64_t,void*,size_t){},false);
   Check(pc->GetAttributes(longNameId)==longName,"Long UTF-8 attribute name was lost in the companion");
  }
  {
   const auto root=VolumeCoord::Wide((LPCSTR)windows);
   const uint64_t boundary=1ULL<<32;
   auto sparse=[&](uint64_t length,uint8_t value,uint64_t offset,void* bytes,size_t size){
    memset(bytes,0,size);
    if(offset<=length-8&&length-8-offset<size)
     static_cast<uint8_t*>(bytes)[length-8-offset]=value;
   };
   for(auto length:{boundary-16,boundary+137}){
    MacBrowser::Entry e;e.name=length<boundary?u"Standard large fork":u"Wide resource fork";
    e.forks[0].size=4;e.forks[1].size=length;e.finder[0]=0x7f;
    const auto id=pc->Put(2,e,[&](unsigned f,uint64_t at,void* p,size_t n){
      if(f)sparse(length,0x71,at,p,n);else memset(p,'D',n);
    },false);
    Check(pc->Stat(id).forks[1].size==length&&pc->Stat(id).finder==e.finder,"Windows truncated a large resource fork");
    uint8_t tail[8]{};
    pc->Read(id,1,length-8,tail,sizeof(tail));
    Check(tail[0]==0x71&&tail[7]==0,"Readback of large resource fork failed");
    pc->Close();
    const auto companion=root+L"\\%"+MacBrowser::Wide(e.name);
    auto disk=HfsPlus::OpenImage(companion.c_str(),false);
    const auto signature=HfsPlus::Disk::Read(*disk,0,8);
    if(length<boundary)
     Check(HfsPlus::Disk::U32(signature.data())==0x00051607&&disk->Size()>boundary,
           "Standard AppleDouble unnecessarily rejected an end offset above 4 GiB");
    else
     Check(!memcmp(signature.data(),"HFVAD641",8),"Oversize fork did not use the 64-bit companion");
    disk.reset();
    if(length>boundary){
     pc->SetAttributes(id,{{u"user.WithLargeFork",{1,2,3}}});
     Check(pc->GetAttributes(id).at(u"user.WithLargeFork")==HfsPlus::AttributeData({1,2,3}),
           "Failed to stage metadata with large resource fork");
     pc->Read(id,1,length-8,tail,sizeof(tail));
     Check(tail[0]==0x71&&tail[7]==0,"Metadata edit damaged the large resource fork");
    }
    pc->Close();
    pc->Remove(id);
   }
   MacBrowser::Entry e;e.name=u"Wide attribute";e.forks[0].size=3;e.attributesKnown=true;
   const auto length=boundary+23;
   e.attributes.emplace(u"user.Huge",HfsPlus::AttributeData::Snapshot(length,[&](uint64_t at,void* p,size_t n){
       sparse(length,0xc3,at,p,n);
   }));
   const auto id=pc->Put(2,e,[](unsigned,uint64_t,void* p,size_t n){memset(p,'a',n);},false);
   const auto restored=pc->GetAttributes(id).at(u"user.Huge");
   uint8_t last[8]{};
   restored.Read(length-8,last,sizeof(last));
   Check(restored.size()==length&&restored.FileBacked()&&last[0]==0xc3&&last[7]==0,
         "64-bit companion did not preserve the huge attribute");
   pc->Close();
   const auto companion=root+L"\\%Wide attribute";
   {
    const auto handle=CreateFileW(companion.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(handle!=INVALID_HANDLE_VALUE,"Open wide companion corruption fixture");
    LARGE_INTEGER at{};at.QuadPart=64;uint8_t bad[8]{};DWORD written=0;
    const bool changed=SetFilePointerEx(handle,at,nullptr,FILE_BEGIN)&&WriteFile(handle,bad,sizeof(bad),&written,nullptr)&&written==sizeof(bad);
    CloseHandle(handle);
    Check(changed,"Write invalid wide companion extent");
   }
   bool rejected=false;
   try{pc->GetAttributes(id);}catch(const HfsPlus::Error&){rejected=true;}
   Check(rejected,"Wide companion accepted an attribute overlapping its header");
   Check(DeleteFileW(companion.c_str())&&DeleteFileW((root+L"\\Wide attribute").c_str()),
         "Remove the intentionally corrupted test pair");
   fprintf(report,"PASS: standard AppleDouble can end above 4 GiB. Wide companions preserve resource forks and streamed attributes above 4 GiB, metadata edits and sparse tails, and reject corrupt extents.\n");
   fflush(report);
  }
  {MacBrowser::Entry e;e.name=u"ExternalChange";e.forks[0].size=3;auto id=pc->Put(2,e,[](unsigned,uint64_t,void* p,size_t n){memset(p,'A',n);},false);pc->Close();auto path=std::wstring(VolumeCoord::Wide((LPCSTR)windows))+L"\\ExternalChange",saved=path+L"-saved";bool replaced=false,failed=false;try{pc->Put(2,e,[&](unsigned,uint64_t,void* p,size_t n){if(!replaced){Check(MoveFileW(path.c_str(),saved.c_str()),"replace staged destination");HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"create external replacement");DWORD wrote=0;Check(WriteFile(h,"EXTERNAL",8,&wrote,nullptr)&&wrote==8,"write external replacement");CloseHandle(h);replaced=true;}memset(p,'B',n);},true);}catch(const std::exception&){failed=true;}pc->Close();HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);char bytes[8]{};DWORD got=0;Check(h!=INVALID_HANDLE_VALUE&&ReadFile(h,bytes,8,&got,nullptr)&&got==8&&!memcmp(bytes,"EXTERNAL",8)&&failed,"Staged copy overwrote an external replacement");CloseHandle(h);}
  {struct Reset{~Reset(){MacBrowser::windowsPublicationFault={};}}reset;auto root=VolumeCoord::Wide((LPCSTR)windows);
   auto raw=[&](const std::wstring& path){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);Check(h!=INVALID_HANDLE_VALUE,"open recovery fixture");LARGE_INTEGER size{};Check(GetFileSizeEx(h,&size)&&size.QuadPart<1048576,"recovery fixture bounds");HfsPlus::Bytes b(size_t(size.QuadPart));DWORD got=0;bool ok=ReadFile(h,b.data(),DWORD(b.size()),&got,nullptr)&&got==b.size();CloseHandle(h);Check(ok,"read recovery fixture");return b;};
   for(unsigned mode=0;mode<3;++mode){MacBrowser::Entry e;e.name=u"Publication"+MacBrowser::U16(std::to_wstring(mode));e.forks[0].size=3;e.forks[1].size=4;e.finder[0]=0x7e;auto id=pc->Put(2,e,[](unsigned f,uint64_t,void* p,size_t n){memset(p,f?'R':'A',n);},false);pc->Close();auto path=root+L"\\"+MacBrowser::Wide(e.name),companion=root+L"\\%"+MacBrowser::Wide(e.name);auto oldData=raw(path),oldResource=raw(companion);bool failed=false;std::string message;
    MacBrowser::windowsPublicationFault=[&](const char* phase){if((mode==0&&!strcmp(phase,"data-published"))||(mode>0&&!strcmp(phase,"resource-published"))){if(mode==2){Check(MoveFileW(path.c_str(),(path+L"-moved").c_str()),"move new output before rollback");HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"create unrelated rollback occupant");DWORD wrote=0;Check(WriteFile(h,"FOREIGN",7,&wrote,nullptr)&&wrote==7,"write rollback occupant");CloseHandle(h);}throw HfsPlus::Error("Injected publication failure");}};
    Storage::OutcomeEvidence evidence;
    try{pc->Put(2,e,[](unsigned f,uint64_t,void* p,size_t n){memset(p,f?'S':'B',n);},true);}catch(const std::exception& error){failed=true;message=error.what();}MacBrowser::windowsPublicationFault={};pc->Close();Check(failed&&evidence.RollbackState()==(mode<2?Storage::Rollback::Restored:Storage::Rollback::Failed),"Windows rollback disposition did not match publication failure");
    if(mode<2)Check(raw(path)==oldData&&raw(companion)==oldResource,"Failed publication did not restore both original forks");
    else{Check(raw(path)==HfsPlus::Bytes({'F','O','R','E','I','G','N'}),"Rollback deleted an unrelated replacement");unsigned preserved=0;WIN32_FIND_DATAW found{};auto find=FindFirstFileW((root+L"\\HFV*.tmp").c_str(),&found);if(find!=INVALID_HANDLE_VALUE){do{auto backup=root+L"\\"+found.cFileName;auto b=raw(backup);if(b==oldData||b==oldResource){++preserved;Check(message.find(BrowserPresentation::Utf8(MacBrowser::U16(BrowserPresentation::WindowsPath(backup))))!=std::string::npos,"Rollback did not identify its exact backup");}}while(FindNextFileW(find,&found));FindClose(find);}Check(preserved==2,"Unrecoverable publication did not retain both original backups");}
   }
   {MacBrowser::Entry e;e.name=u"MetadataPrimary";e.forks[0].size=3;auto id=pc->Put(2,e,[](unsigned,uint64_t,void* p,size_t n){memset(p,'A',n);},false);pc->Close();auto path=root+L"\\MetadataPrimary",companion=root+L"\\%MetadataPrimary";auto original=raw(companion);bool failed=false;MacBrowser::windowsPublicationFault=[&](const char* phase){if(strcmp(phase,"metadata-staged"))return;Check(MoveFileW(path.c_str(),(path+L"-saved").c_str()),"move primary during metadata staging");HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"create metadata replacement");DWORD n=0;Check(WriteFile(h,"FOREIGN",7,&n,nullptr)&&n==7,"write metadata replacement");CloseHandle(h);};try{pc->SetAttributes(id,{{u"user.New",{1,2,3}}});}catch(const std::exception&){failed=true;}MacBrowser::windowsPublicationFault={};pc->Close();Check(failed&&raw(path)==HfsPlus::Bytes({'F','O','R','E','I','G','N'})&&raw(companion)==original,"Metadata staging changed a substituted primary or companion");}
   MacBrowser::Entry held;held.name=u"WriterGuard";held.forks[0].size=3;pc->Put(2,held,[](unsigned,uint64_t,void* p,size_t n){memset(p,'A',n);},false);pc->Close();auto path=root+L"\\WriterGuard";bool blocked=false;pc->Put(2,held,[&](unsigned,uint64_t,void* p,size_t n){auto h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);blocked=h==INVALID_HANDLE_VALUE;if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);memset(p,'B',n);},true);Check(blocked,"Replacement did not exclude competing in-place writers");pc->Close();
  }
  {MacBrowser::Entry e;e.name=u"File";e.forks[0].size=3;pc->Put(2,e,[](unsigned,uint64_t,void* p,size_t n){memset(p,'A',n);},false);pc->Close();FileOperations::Request copy;copy.background=true;FileOperations::Location from;from.SetMounted(native,file);from.cut=true;copy.Copy().sources={from};copy.Copy().destination.SetMounted(pc,2);auto op=FileOperations::Submit(copy);Until([&]{return !op.Job()->conflict().empty()||op.Job()->done();});Check(!op.Job()->done(),"No overwrite confirmation in race test");auto path=VolumeCoord::Wide((LPCSTR)windows)+L"\\File";Check(MoveFileW(path.c_str(),(path+L"-before-prompt").c_str()),"swap destination while confirmation is pending");HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(h!=INVALID_HANDLE_VALUE,"create pending-prompt replacement");DWORD wrote=0;Check(WriteFile(h,"PROMPT",6,&wrote,nullptr)&&wrote==6,"write prompt replacement");CloseHandle(h);op.Job()->resolve(Storage::ConflictChoice::Replace);Until([&]{return op.Job()->done();});Check(op.Job()->result().code&&native->Stat(file).forks[0].size==8192,"Changed destination was overwritten or move removed its source");pc->Close();h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);char bytes[6]{};DWORD got=0;Check(h!=INVALID_HANDLE_VALUE&&ReadFile(h,bytes,6,&got,nullptr)&&got==6&&!memcmp(bytes,"PROMPT",6),"Overwrite approval applied to a different file");CloseHandle(h);}
  fprintf(report,"PASS: destination substitution during staging/overwrite confirmation is refused; competing in-place writers are excluded; both forks restore after data/resource publication failures; unrelated rollback occupants survive while both original backups and exact recovery paths are retained; metadata primary-file substitution is refused.\n");
  fprintf(report,"PASS: HFS+/HFSX and Windows AppleDouble roundtrips retain empty/binary/case-distinct attributes, Mac ACL bytes, both forks and nested directory metadata. Folder rename/deletion maintains companions; unsupported destinations reject before source removal.\n");
  int mount=session.MacBrowser().AttachTestVolume(native,"attributes");Check(session.MacBrowser().ShowTestFolder(mount,2),"show attributed volume");FileOperations::Location selected;selected.SetMounted(native,file);selected.expectedIdentity=MacBrowser::EntryIdentity(std::to_string(file));
  auto submit=[&](FileOperations::Request request,bool success=true){request.background=true;auto op=FileOperations::Submit(request);Until([&]{return op.Job()->done();});if((op.Job()->result().code==0)!=success)fprintf(report,"Unexpected result: %s\n",op.Job()->result().message.c_str());Check((op.Job()->result().code==0)==success,"Attribute operation result");return op.Snapshot();};

  {auto value=HfsPlus::AttributeData::Snapshot((70ULL<<20)+17,[](uint64_t at,void* p,size_t n){memset(p,0,n);if(at==0&&n)static_cast<BYTE*>(p)[0]=0x91;});auto path=FileOperations::Location::Windows(VolumeCoord::Wide(std::string((LPCSTR)host)+"\\large-value.bin"));submit(FileOperations::ExportAttributeValue{path,value});FileOperations::ImportAttributeValue input{path,u"user.Import"};auto imported=submit(input).attributes.at(input.name);Check(imported.FileBacked()&&imported==value,"Large binary import/export was not streamed or changed bytes");}
  {FileOperations::Request conversion;conversion = FileOperations::LegacyConversion{};conversion.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)windows,(LPCSTR)(windows+"\\Roundtrip"));conversion.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)classic,"NoAttributes:Imported");conversion.Legacy().options.input=conversion.Legacy().options.output='r';conversion.Legacy().options.appledouble=true;submit(conversion,false);
   conversion.Legacy().destination=FileOperations::Location::Classic((LPCSTR)classic,"NoAttributes:");conversion.Legacy().target=FileOperations::ConversionTarget::Folder;submit(conversion,false);auto v=MacBrowser::OpenClassic((LPCSTR)classic,"NoAttributes:",false);Check(v->List().empty()&&pc->GetAttributes(exported)==attrs,"Legacy conversion discarded attributes or created partial output");v->Close();}
  // Direct fixture writes model an independent edit. Let any asynchronous
  // browser refresh release its shared lease before taking the fixture writer.
  auto reserveFixtureWrite=[&]{
   std::unique_ptr<VolumeCoord::Lease> lease;
   Until([&]{
    if(lease)return true;
    try{lease=std::make_unique<VolumeCoord::Lease>(
        std::vector<VolumeCoord::Access>{{native->ResourceKey(),VolumeCoord::Mode::Write}});}
    catch(const VolumeCoord::Busy&){return false;}
    return true;
   });
   return lease;
  };
  auto writeFixtureAttributes=[&](const HfsPlus::Attributes& attributes){
   auto lease=reserveFixtureWrite();
   native->SetAttributes(file,attributes);
  };
  auto readFixtureAttributes=[&]{
   std::optional<HfsPlus::Attributes> attrs;
   Until([&]{
    if(attrs)return true;
    try{attrs=native->GetAttributes(file);}
    catch(const VolumeCoord::Busy&){return false;}
    return true;
   });
   return std::move(*attrs);
  };
  FileOperations::InspectProperties inspect{selected};auto info=submit(inspect);Check(info.capabilities.attributes&&info.attributes==attrs,"Properties omitted attributes");
  Check(MetadataDialogs::Show(selected,frame),"show attribute Properties");Until([]{return !BackgroundCopies::Active();});auto summary=MetadataDialogs::Window();Check(summary!=nullptr,"missing attribute summary");SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto editor=MetadataDialogs::Window();Check(editor&&editor!=summary&&IsWindowEnabled(GetDlgItem(editor,IDOK)),"missing writable attribute editor");
  SetDlgItemTextW(editor,IDC_ATTR_NAME,L"user.Edited");SetDlgItemTextW(editor,IDC_ATTR_VALUE,L"DE AD 00 BE EF");SendMessage(editor,WM_COMMAND,IDC_ATTR_SET,0);SendMessage(editor,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&readFixtureAttributes().at(u"user.Edited")==HfsPlus::Bytes({0xde,0xad,0,0xbe,0xef})&&frame->IsWindowEnabled(),"Properties edit failed/disabled browser");MetadataDialogs::Shutdown();
  FileOperations::EditAttributes edit{selected,attrs,{}};submit(edit,false);Check(readFixtureAttributes().count(u"user.Edited"),"Stale attribute request discarded newer metadata");
  {Check(MetadataDialogs::Show(selected,frame),"open Properties before concurrent attribute update");Until([]{return !BackgroundCopies::Active();});auto summary=MetadataDialogs::Window();SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto editor=MetadataDialogs::Window();auto fresh=readFixtureAttributes();fresh[u"user.Concurrent"]={7,8};writeFixtureAttributes(fresh);SendMessage(editor,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});Check(BackgroundCopies::LastResult().code&&readFixtureAttributes()==fresh,"Stale Properties editor overwrote a concurrent attribute change");auto progress=BackgroundCopies::DialogWindow();if(IsWindow(progress))SendMessage(progress,WM_COMMAND,IDCANCEL,0);MetadataDialogs::Shutdown();}
  CString raw=host+"\\value.bin";auto path=FileOperations::Location::Windows(VolumeCoord::Wide((LPCSTR)raw));FileOperations::ExportAttributeValue exportValue{path,attrs.at(u"com.apple.system.Security")};submit(exportValue);submit(exportValue,false);FileOperations::ImportAttributeValue read{path,u"com.apple.system.Security"};Check(submit(read).attributes.at(read.name)==security,"Binary attribute import/export changed bytes");
  auto original=readFixtureAttributes();edit.original=original;edit.desired=attrs;auto invalid=security;invalid.pop_back();edit.desired[u"com.apple.system.Security"]=invalid;submit(edit,false);Check(readFixtureAttributes()==original,"Invalid ACL changed existing attributes");edit.desired=attrs;edit.desired.erase(u"com.apple.system.Security");submit(edit);Check(!readFixtureAttributes().count(u"com.apple.system.Security"),"ACL removal failed");

  // Queue/cancel uses the same atomic volume coordination as other mutations.
  {auto held=reserveFixtureWrite();auto before=readFixtureAttributes();edit.original=before;edit.desired={};FileOperations::Request queued=edit;queued.background=true;auto blocked=FileOperations::Submit(queued);
   FileOperations::Request unrelated;unrelated = FileOperations::CreateFolder{};unrelated.background=true;unrelated.Get<FileOperations::CreateFolder>()->parent.SetMounted(destination,2);unrelated.Get<FileOperations::CreateFolder>()->name=u"Unrelated";auto other=FileOperations::Submit(unrelated);Until([&]{return other.Job()->done();});Check(!other.Job()->result().code&&!blocked.Job()->done(),"Attribute mutation blocked unrelated volume or bypassed its own lease");blocked.Job()->cancel();Until([&]{return blocked.Job()->done();});held.reset();Check(readFixtureAttributes()==before,"Canceled attribute mutation changed metadata");}
  // Edit three pages without materializing the value; navigation stages each
  // page and the original snapshot must remain immutable after Apply.
  {auto original=readFixtureAttributes();auto page=HfsPlus::AttributeData::Snapshot(131089,[](uint64_t,void* p,size_t n){memset(p,0,n);});auto desired=original;desired[u"user.Page"]=page;writeFixtureAttributes(desired);Check(MetadataDialogs::Show(selected,frame),"show paged attributes");Until([]{return !BackgroundCopies::Active();});auto summary=MetadataDialogs::Window();SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto window=MetadataDialogs::Window();auto position=std::distance(desired.begin(),desired.find(u"user.Page"));SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,position,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);Check(IsWindowEnabled(GetDlgItem(window,IDC_ATTR_NEXT)),"Large attribute has no page navigation");
   auto changeFirst=[&](const wchar_t* pair){auto control=GetDlgItem(window,IDC_ATTR_VALUE);auto n=GetWindowTextLengthW(control);std::wstring text(n+1,0);GetWindowTextW(control,&text[0],n+1);text.resize(n);Check(text.size()>=2&&text.size()<262144,"Page text grew with entire attribute");text.replace(0,2,pair);SetWindowTextW(control,text.c_str());};changeFirst(L"AB");SetDlgItemTextW(window,IDC_ATTR_OFFSET,L"65536");SendMessage(window,WM_COMMAND,IDC_ATTR_GO,0);changeFirst(L"CD");SendMessage(window,WM_COMMAND,IDC_ATTR_NEXT,0);changeFirst(L"EF");SendMessage(window,WM_COMMAND,IDC_ATTR_SET,0);SendMessage(window,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code,"Paged attribute Apply failed");auto got=readFixtureAttributes().at(u"user.Page");for(auto item:{std::make_pair(uint64_t(0),BYTE(0xab)),std::make_pair(uint64_t(65536),BYTE(0xcd)),std::make_pair(uint64_t(131072),BYTE(0xef))}){BYTE actual=0;got.Read(item.first,&actual,1);Check(actual==item.second,"Paged edit lost another page");page.Read(item.first,&actual,1);Check(actual==0,"Paged editor modified original snapshot");}MetadataDialogs::Shutdown();}
  {auto detached=Image(host+"\\dialog-lifetime.dsk");auto id=Put(*detached,u"Dialog");detached->SetAttributes(id,{{u"b",{1}},{u"c",{2}},{u"\u4e2d\u6587",{3}}});std::weak_ptr<MacBrowser::Volume> weak=detached;FileOperations::Location location;location.SetMounted(detached,id);Check(MetadataDialogs::Show(location,frame),"open detached dialog");Until([]{return !BackgroundCopies::Active();});auto summary=MetadataDialogs::Window();SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto window=MetadataDialogs::Window();location={};detached.reset();SendMessage(summary,WM_COMMAND,IDCANCEL,0);Check(!weak.expired(),"Child editor lost its source");SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,0,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);SetDlgItemTextW(window,IDC_ATTR_NAME,L"a");SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,1,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);Check(BrowserPresentation::Text(GetDlgItem(window,IDC_ATTR_NAME))==L"c","Staging an inserted attribute selected the wrong row");SetDlgItemTextW(window,IDC_ATTR_VALUE,L"GG");SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,1,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);Check(!MetadataDialogs::LastError().empty()&&BrowserPresentation::Text(GetDlgItem(window,IDC_ATTR_NAME))==L"c"&&SendDlgItemMessage(window,IDC_ATTR_LIST,LB_GETCURSEL,0,0)==2,"Invalid edit changed the selected attribute");SetDlgItemTextW(window,IDC_ATTR_VALUE,L"02");SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,3,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);Check(BrowserPresentation::Text(GetDlgItem(window,IDC_ATTR_NAME))==L"\u4e2d\u6587"&&IsWindowUnicode(GetDlgItem(window,IDC_ATTR_NAME)),"Attribute editor lost a Unicode attribute name");SendMessage(window,WM_COMMAND,IDCANCEL,0);Pump();Check(weak.expired(),"Closed metadata dialogs retained their source volume");MetadataDialogs::Shutdown();}
  {auto values=readFixtureAttributes();values[u"\u4e2d\u6587"]={3};writeFixtureAttributes(values);Check(MetadataDialogs::Show(selected,frame),"open Unicode attribute editor");Until([]{return !BackgroundCopies::Active();});auto summary=MetadataDialogs::Window();SendMessage(summary,WM_COMMAND,IDC_ATTR_OPEN,0);auto window=MetadataDialogs::Window();auto index=std::distance(values.begin(),values.find(u"\u4e2d\u6587"));SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,index,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);Check(BrowserPresentation::Text(GetDlgItem(window,IDC_ATTR_NAME))==L"\u4e2d\u6587","Unicode attribute name did not display exactly");SetDlgItemTextW(window,IDC_ATTR_VALUE,L"04");SendDlgItemMessage(window,IDC_ATTR_LIST,LB_SETCURSEL,0,0);SendMessage(window,WM_COMMAND,MAKEWPARAM(IDC_ATTR_LIST,LBN_SELCHANGE),0);SendMessage(window,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});auto actual=readFixtureAttributes();Check(!BackgroundCopies::LastResult().code&&actual.size()==values.size()&&actual.at(u"\u4e2d\u6587")==HfsPlus::Bytes({4}),"Unicode navigation/apply changed attribute keys or lost edits");MetadataDialogs::Shutdown();}
  {auto detached=Image(host+"\\nested-dialog-close.dsk");auto id=Put(*detached,u"Nested");std::weak_ptr<MacBrowser::Volume> weak=detached;FileOperations::Location source;source.SetMounted(detached,id);Check(MetadataDialogs::Show(source,frame),"open nested-close dialog");Until([]{return !BackgroundCopies::Active();});auto window=MetadataDialogs::Window();source={};detached.reset();Check(MetadataDialogs::CloseDuringDispatch(window)==1&&!IsWindow(window)&&weak.expired(),"Dialog was deleted during its dispatch or retained its storage");}
  fprintf(report,"PASS: inserted attributes retain name-based selection, invalid edits retain their row/draft, closed parent/child dialogs release storage references, and nested shutdown defers C++ dialog destruction until dispatch returns.\n");
  fprintf(report,"PASS: background attribute inspection/editing, binary value import/export, actual hidden modeless Properties editor, stale-editor rejection, malformed ACL rejection and ACL removal; browser stays enabled and conflicting output is retained. No physical media/computer control.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last result: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});MetadataDialogs::Shutdown();ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunMfsWriteBrowserTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\mfs-write-browser-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 try{
  auto create=[&](const CString& path,bool dc42){uint64_t data=400*1024,total=data+(dc42?84+data/512*12:0);{CFile f;Check(f.Open(path,CFile::modeCreate|CFile::modeWrite),"create MFS test image");f.SetLength(total);}MfsImageSource source;std::string error;Check(source.Open(VolumeCoord::Wide((LPCSTR)path).c_str(),error,true),error.c_str());Check(MfsImage::Format(source,"MFS write",data,dc42,error),error.c_str());};
  CString a=host+"\\raw.dsk",b=host+"\\diskcopy.dc42";create(a,false);create(b,true);auto first=MacBrowser::OpenImage(VolumeCoord::Wide((LPCSTR)a)),second=MacBrowser::OpenImage(VolumeCoord::Wide((LPCSTR)b));Check(first&&second&&first->Writable()&&second->Writable()&&!first->Supports().createFolders,"MFS main adapters not writable/flat");
  {MfsImageSource conflict;std::string error;Check(!conflict.Open(VolumeCoord::Wide((LPCSTR)a).c_str(),error,true)&&!conflict.Open(VolumeCoord::Wide((LPCSTR)a).c_str(),error),"MFS writer allowed conflicting independent instance");}
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto one=app.CreateBrowser(FALSE),two=app.CreateBrowser(FALSE);Check(one&&two,"create MFS browser windows");auto& left=one->GetBrowserSession();auto& right=two->GetBrowserSession();int l=left.MacBrowser().AttachTestVolume(first,"mfs"),r=right.MacBrowser().AttachTestVolume(first,"mfs");Check(left.MacBrowser().ShowTestFolder(l,2)&&right.MacBrowser().ShowTestFolder(r,2)&&left.MacBrowser().Writable()&&!left.MacBrowser().CanCreateFolder(),"MFS root file/folder command capabilities");
  auto source=Image(host+"\\source.dsk");auto incoming=Put(*source,u"File");FileOperations::Request copy;FileOperations::Location from;from.SetMounted(source,incoming);copy.Copy().sources={from};copy.Copy().destination.SetMounted(first,2);
  Check(BackgroundCopies::Start({copy},one,true),"start MFS background import");Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code,"MFS background import failed");Until([&]{return Row(left,L"File")>=0&&Row(right,L"File")>=0;});auto file=Find(*first,u"File");Check(file&&first->Stat(file).forks[0].size==8192&&first->Stat(file).forks[1].size==31,"MFS imported fork sizes");
  // Same name and a different version must create a separate entry.
  auto version=first->Stat(file);version.version=3;auto versionId=first->Put(2,version,[&](unsigned fork,uint64_t at,void* p,size_t n){source->Read(incoming,fork,at,p,n);},false);Check(versionId!=file&&first->List().size()==2,"MFS versions collided");
  Storage::Job transfer;Check(MacBrowser::Copy(*first,versionId,*second,2,transfer),"MFS versioned both-fork transfer");auto copied=second->List();Check(copied.size()==1&&copied.front().version==3&&copied.front().finder==version.finder,"MFS version/Finder copy fidelity");
  // Overwriting the same version still goes through the ordinary overwrite UI.
  copy.Copy().sources.front().SetMounted(first,versionId);copy.Copy().destination.SetMounted(second);Check(BackgroundCopies::Start({copy},two,true),"start MFS overwrite");Until([]{auto pending=FileOperations::Pending();return !pending.empty()&&!pending.front().Job()->conflict().empty();});auto overwrite=FileOperations::Pending().front().Job();overwrite->resolve(Storage::ConflictChoice::Replace);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&second->List().size()==1,"MFS overwrite duplicated/version-changed entry");
  FileOperations::Location selected;selected.SetMounted(first,file);auto submit=[&](FileOperations::Request request,bool success=true){request.background=true;auto op=FileOperations::Submit(request);Until([&]{return op.Job()->done();});if((op.Job()->result().code==0)!=success)fprintf(report,"Unexpected MFS job result: %s\n",op.Job()->result().message.c_str());Check((op.Job()->result().code==0)==success,"MFS job returned wrong result");return op.Snapshot();};
  Check(MetadataDialogs::Show(selected,one),"open MFS properties");Until([]{return !BackgroundCopies::Active();});auto editor=MetadataDialogs::Window();Check(editor&&GetDlgItem(editor,IDC_MFS_VERSION),"MFS properties editor missing");SetDlgItemTextW(editor,IDC_HPLUS_NAME,L"Renamed \u00e9");SetDlgItemInt(editor,IDC_MFS_VERSION,7,FALSE);SendMessage(editor,WM_COMMAND,IDOK,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&first->Stat(file).version==7&&first->Stat(file).name==u"Renamed \u00e9","MFS metadata editor lost name/version/stable ID");MetadataDialogs::Shutdown();Until([&]{return Row(left,L"Renamed \u00e9 [version 7]")>=0&&Row(right,L"Renamed \u00e9 [version 7]")>=0;});
  auto stale=first->Stat(file);FileOperations::EditBasicMetadata metadata{selected,stale,stale};metadata.desired.version=9;first->Rename(file,2,u"Changed outside editor");submit(metadata,false);Check(first->Stat(file).version==7,"stale MFS properties overwrote new state");
  FileOperations::Request remove = FileOperations::DeleteEntries{{selected}};auto free=first->FreeBytes();submit(remove);Check(first->Stat(versionId).version==3&&first->FreeBytes()>free,"MFS delete removed wrong version or leaked chains");Until([&]{return Row(left,L"Renamed \u00e9 [version 7]")<0&&Row(right,L"Renamed \u00e9 [version 7]")<0;});
  // Directory and xattr inputs must fail in preflight, before any output.
  auto folder=source->MakeDirectory(2,u"Folder");copy.Copy().sources.front().SetMounted(source,folder);copy.Copy().destination.SetMounted(first);auto count=first->List().size();submit(copy,false);Check(first->List().size()==count,"MFS accepted a directory or produced partial output");source->SetAttributes(incoming,{{u"user.Keep",{1,2,3}}});copy.Copy().sources.front().SetId(incoming);submit(copy,false);Check(first->List().size()==count&&source->GetAttributes(incoming).count(u"user.Keep"),"MFS lossy attribute copy changed destination/source");
  fprintf(report,"PASS: writable raw/DC42 images in independent main windows, background both-fork import, same-name versions, overwrite prompts, properties name/version editing, stable IDs, stale-editor rejection, delete/reclamation and cross-window refresh; flat-folder/xattr preflight rejection; exclusive handles. No hardware or computer control.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last result: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});MetadataDialogs::Shutdown();ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}

int RunCompressionWriteBrowserTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\compression-write-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto frame=app.CreateBrowser(FALSE);Check(frame!=nullptr,"create compression browser");auto target=Image(host+"\\target.dsk"),input=Image(host+"\\input.dsk");
  MacBrowser::Entry e;e.name=u"File";e.forks[0].size=150000;auto pattern=[](unsigned,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE('A'+(at+i)%3);};auto id=target->Put(2,e,pattern,false);target->SetAttributes(id,{{u"user.keep",{1,2,3}}});
  FileOperations::Location selected;selected.SetMounted(target,id);
  auto submit=[&](FileOperations::Request r,bool success=true){auto op=FileOperations::Submit(std::move(r));Until([&]{if(!op.Job()->conflict().empty())op.Job()->resolve(Storage::ConflictChoice::Replace);return op.Job()->done();});Check((op.Job()->result().code==0)==success,"compression operation result");return op;};
  auto verify=[&](uint64_t size){auto now=target->Stat(id);Check(now.forks[0].size==size,"browser logical compressed size");HfsPlus::Bytes got(size_t(size),0),expected(got.size());target->Read(id,0,0,got.data(),got.size());pattern(0,0,expected.data(),expected.size());Check(got==expected,"browser compressed contents");};
  Check(MetadataDialogs::Show(selected,frame),"open compression properties");Until([]{return !BackgroundCopies::Active();});auto window=MetadataDialogs::Window();Check(window&&IsWindowEnabled(GetDlgItem(window,IDC_FILE_COMPRESSION)),"missing compression button");SendMessage(window,WM_COMMAND,IDC_FILE_COMPRESSION,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&target->Stat(id).compressed&&frame->IsWindowEnabled(),"background compression failed or disabled owner");verify(150000);Check(target->GetAttributes(id)==HfsPlus::Attributes({{u"user.keep",{1,2,3}}}),"private compression attribute exposed/lost user metadata");MetadataDialogs::Shutdown();
  {
   const auto path=VolumeCoord::Wide((LPCSTR)(host+"\\read-only-properties.dsk"));
   uint32_t readOnlyId;
   {
    auto seed=Image(host+"\\read-only-properties.dsk");
    readOnlyId=seed->Put(2,e,pattern,false);
    seed->SetAttributes(readOnlyId,{{u"user.inspect",{7}}});
   }
   Check(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"mark fixture read-only");
   struct ResetAttributes {
    const std::wstring& path;
    ~ResetAttributes(){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);}
   } reset{path};
   auto readOnly=MacBrowser::OpenBrowserImage(path);
   Check(readOnly&&VolumeSession::VolumeSessionPidForTest(readOnly)&&readOnly->Supports().attributes&&
         !readOnly->Supports().CanEditCompression()&&!readOnly->Supports().CanCreateHardLinks(),
         "read-only worker advertised metadata writes");
   auto item=FileOperations::Location::Mounted(readOnly,readOnlyId);
   Check(MetadataDialogs::Show(item,frame),"open read-only worker Properties");
   Until([]{return !BackgroundCopies::Active();});
   const auto properties=MetadataDialogs::Window();
   Check(properties&&GetDlgItem(properties,IDC_ATTR_OPEN)&&
         !IsWindowEnabled(GetDlgItem(properties,IDC_FILE_COMPRESSION)),
         "read-only Properties did not match worker capabilities");
   MetadataDialogs::Shutdown();
   const auto entry=readOnly->Stat(readOnlyId);
   submit(FileOperations::ChangeCompression{item,entry,readOnly->GetAttributes(readOnlyId),
          HfsPlus::Volume::CompressionMode::Zlib},false);
   Check(!readOnly->Stat(readOnlyId).compressed,"disabled compression could modify the read-only image");
  }
  // Overwrite a compressed target through the real copy job and overwrite handling.
  e.forks[0].size=200001;auto from=input->Put(2,e,pattern,false);FileOperations::Request copy;FileOperations::Location source;source.SetMounted(input,from);copy.Copy().sources={source};copy.Copy().destination.SetMounted(target,2);auto copied=submit(copy);Check(target->Stat(id).compressed,"copy overwrite lost compression");verify(200001);auto progress=copied.Job()->progress();Check(progress.processed==200001&&progress.fileBytes==200001,"encoder second pass counted twice in progress");
  // Stale Properties cannot silently apply a conversion to a changed file.
  FileOperations::ChangeCompression change{selected,target->Stat(id),target->GetAttributes(id),HfsPlus::Volume::CompressionMode::Uncompressed};target->SetAttributes(id,{{u"user.new",{9}}});submit(change,false);Check(target->Stat(id).compressed,"stale compression Properties changed file");
  Check(MetadataDialogs::Show(selected,frame),"open decompression properties");Until([]{return !BackgroundCopies::Active();});window=MetadataDialogs::Window();char label[64]{};GetDlgItemTextA(window,IDC_FILE_COMPRESSION,label,sizeof(label));Check(!strcmp(label,"Decompress"),"wrong compressed Properties action");SendMessage(window,WM_COMMAND,IDC_FILE_COMPRESSION,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&!target->Stat(id).compressed&&frame->IsWindowEnabled(),"background decompression failed");verify(200001);MetadataDialogs::Shutdown();
  // Every new method is selectable through the actual modeless Properties UI.
  for(int choice:{2,3,4,5}){Check(MetadataDialogs::Show(selected,frame),"open codec selector");Until([]{return !BackgroundCopies::Active();});auto properties=MetadataDialogs::Window();Check(SendDlgItemMessage(properties,IDC_COMPRESSION_METHOD,CB_GETCOUNT,0,0)==7,"missing compression choices");SendDlgItemMessage(properties,IDC_COMPRESSION_METHOD,CB_SETCURSEL,choice,0);SendMessage(properties,WM_COMMAND,MAKEWPARAM(IDC_COMPRESSION_METHOD,CBN_SELCHANGE),0);SendMessage(properties,WM_COMMAND,IDC_FILE_COMPRESSION,0);Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code&&target->Stat(id).compressed,"selected background codec failed");verify(200001);MetadataDialogs::Shutdown();}
  Check(MetadataDialogs::Show(selected,frame),"open legacy codec size guard");Until([]{return !BackgroundCopies::Active();});window=MetadataDialogs::Window();SendDlgItemMessage(window,IDC_COMPRESSION_METHOD,CB_SETCURSEL,6,0);SendMessage(window,WM_COMMAND,IDC_FILE_COMPRESSION,0);Until([]{return !BackgroundCopies::Active();});Check(BackgroundCopies::LastResult().code&&target->Stat(id).compressed,"oversized legacy method did not fail safely");verify(200001);MetadataDialogs::Shutdown();
  change.method=HfsPlus::Volume::CompressionMode::Zlib;change.original=target->Stat(id);change.originalAttributes=target->GetAttributes(id);submit(change);Check(target->Stat(id).compressed,"compression job routing");
  FileOperations::Request rename = FileOperations::RenameEntry{selected, u"Renamed", FileOperations::Location::Mounted(target,2)};submit(rename);Check(target->Stat(id).compressed&&target->Stat(id).name==u"Renamed","compressed rename failed");
  // Importing a real resource fork converts to ordinary storage when needed.
  MacBrowser::Entry resource;resource.name=u"Resource";resource.forks[0].size=3;auto resourceId=input->Put(2,resource,[](unsigned,uint64_t,void* p,size_t n){memset(p,0x87,n);},false);FileOperations::ImportResourceFork replace;source.SetId(resourceId);replace={source,selected};submit(replace);verify(200001);Check(!target->Stat(id).compressed&&target->Stat(id).forks[1].size==3,"resource import discarded user fork or logical data");BYTE bytes[3];target->Read(id,1,0,bytes,3);Check(bytes[0]==0x87&&bytes[2]==0x87,"resource import bytes changed");
  FileOperations::Request remove = FileOperations::DeleteEntries{{selected}};submit(remove);Check(!Find(*target,u"Renamed"),"compressed/editable file removal failed");
  fprintf(report,"PASS: actual hidden Properties Compress/Decompress controls, non-modal background jobs/owner restoration, private attribute filtering, compressed overwrite with overwrite decision and exact logical progress, stale editor rejection, compressed rename, genuine resource import fallback and deletion. Images only; no physical devices or computer control.\n");
  frame->SendMessage(WM_CLOSE);
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());result=1;}catch(CException* error){char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: %s\n",text);error->Delete();result=1;}
 BackgroundCopies::enabled=false;MetadataDialogs::Shutdown();fclose(report);return result;
}

int RunOtherHfsPlusBrowserTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\other-hfsplus-browser-report.txt","w");if(!report)return 2;int result=0;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();BackgroundCopies::enabled=true;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 try{
  CString image=host+"\\expanded.dsk",journal=host+"\\external.img";auto wide=VolumeCoord::Wide((LPCSTR)image),external=VolumeCoord::Wide((LPCSTR)journal);auto uuid=HfsPlus::CreateExternalJournalImage(external.c_str(),16*1024*1024);
  std::array<uint64_t,2> dates{HfsPlus::Time::Epoch,HfsPlus::Time::Maximum};uint32_t file;
  {auto disk=HfsPlus::CreateImage(wide.c_str(),8*1024*1024,external.c_str());HfsPlus::Volume::Format(*disk,u"Future",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive,true);HfsPlus::Volume v(*disk,true);v.UseExternalJournal(uuid);file=v.Put(2,u"Future",8192,31,[](unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE(f+at+i);},nullptr,false,nullptr,nullptr,HfsPlus::Volume::CompressionMode::Preserve,&dates);}
  app.InstallBrowserTemplate(RUNTIME_CLASS(CopyDoc),RUNTIME_CLASS(CopyFrame));auto one=app.CreateBrowser(FALSE),two=app.CreateBrowser(FALSE);Check(one&&two,"create other-feature browser windows");Check(one->GetBrowserSession().MacBrowser().OpenImage(wide,external)==1&&two->GetBrowserSession().MacBrowser().OpenImage(wide,external)==1&&one->GetBrowserSession().AwaitNavigationForTest()&&two->GetBrowserSession().AwaitNavigationForTest(),"normal browser open with paired journal failed");auto native=MacBrowser::OpenImage(wide,external);auto e=native->Stat(file);Check(e.created==dates[0]&&e.modified==dates[1],"browser timestamp truncation");auto row=BrowserDirectory::FromMac(e,native,native->Supports());Check(row.created==(dates[0]+9561628800ULL)*10000000ULL&&row.modified==(dates[1]+9561628800ULL)*10000000ULL,"display timestamp truncation");
  {VolumeCoord::Lease held({{native->ResourceKey(),VolumeCoord::Mode::Write}});Check(!VolumeCoord::Available({{VolumeCoord::PathKey(external),VolumeCoord::Mode::Read}}),"filesystem operation did not reserve its journal");Check(VolumeCoord::Available({{VolumeCoord::PathKey(wide+L".unrelated"),VolumeCoord::Mode::Read}}),"external journal blocked unrelated files");}
  CString windows=host+"\\windows";Check(CreateDirectoryA(windows,nullptr),"create future export folder");auto pc=MacBrowser::OpenWindows(VolumeCoord::Wide((LPCSTR)windows));Storage::Job job;Check(MacBrowser::Copy(*native,file,*pc,2,job),"future Windows export failed");pc->Close();auto exported=Find(*pc,u"Future");auto metadata=pc->Stat(exported);Check(metadata.created==dates[0]&&metadata.modified==dates[1],"Windows timestamp roundtrip lost 2106 date");pc->Close();
  auto targetPath=VolumeCoord::Wide((LPCSTR)(host+"\\roundtrip.dsk"));{auto disk=HfsPlus::CreateImage(targetPath.c_str(),8*1024*1024);HfsPlus::Volume::Format(*disk,u"Roundtrip",true,HfsPlus::Volume::FormatKind::HfsPlus,true);}auto target=MacBrowser::OpenImage(targetPath);Storage::Job import;Check(MacBrowser::Copy(*pc,exported,*target,2,import),"expanded native import failed");auto roundtrip=target->Stat(Find(*target,u"Future"));Check(roundtrip.created==dates[0]&&roundtrip.modified==dates[1],"2106 date changed on reimport");
  MacBrowser::Entry tooLate=e;tooLate.name=u"Beyond 2106";tooLate.created=tooLate.modified=HfsPlus::Time::Maximum+100;tooLate.datesValid=true;auto lateId=pc->Put(2,tooLate,[](unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE(f+at+i);},false);pc->Close();Check(pc->Stat(lateId).modified==tooLate.modified,"Windows source truncated a date after 2106");bool lateRefused=false;try{Storage::Job move;MacBrowser::Copy(*pc,lateId,*target,2,move,true);}catch(const std::exception&){lateRefused=true;}Check(lateRefused&&!Find(*target,u"Beyond 2106")&&pc->Stat(lateId).modified==tooLate.modified,"Out-of-range Windows timestamp was silently dropped");pc->Close();
  auto legacy=Image(host+"\\legacy.dsk");bool refused=false;try{Storage::Job move;MacBrowser::Copy(*native,file,*legacy,2,move,true);}catch(const std::exception&){refused=true;}Check(refused&&!Find(*legacy,u"Future")&&native->Stat(file).modified==dates[1],"Legacy timestamp refusal changed source or target");
  CString classic=host+"\\classic.dsk";{CFile f;Check(f.Open(classic,CFile::modeCreate|CFile::modeWrite),"create classic date image");f.SetLength(1474560);}char label[]="ClassicDates";Check(hfs_format(const_cast<char*>((LPCSTR)classic),0,label)==0,"format classic date image");auto old=MacBrowser::OpenClassic((LPCSTR)classic,"ClassicDates:",true);refused=false;try{Storage::Job move;MacBrowser::Copy(*native,file,*old,2,move,true);}catch(const std::exception&){refused=true;}Check(refused&&old->List().empty()&&native->Stat(file).modified==dates[1],"Classic HFS date refusal lost source");old->Close();
  FileOperations::Request conversion;conversion = FileOperations::LegacyConversion{};conversion.Legacy().source=FileOperations::Location::FromLegacy((LPCSTR)windows,(LPCSTR)(windows+"\\Future"));conversion.Legacy().destination=FileOperations::Location::FromLegacy((LPCSTR)classic,"ClassicDates:Converted");conversion.Legacy().options.input=conversion.Legacy().options.output='r';conversion.Legacy().destination=FileOperations::Location::Classic((LPCSTR)classic,"ClassicDates:");conversion.Legacy().target=FileOperations::ConversionTarget::Folder;auto converted=FileOperations::Submit(conversion);Until([&]{return converted.Job()->done();});Check(converted.Job()->result().code!=0&&converted.Job()->result().message.find("dates after")!=std::string::npos,"Legacy converter truncated a future date");
  FileOperations::Location selected;selected.SetMounted(native,file);FileOperations::Request rename = FileOperations::RenameEntry{selected, u"Updated", FileOperations::Location::Mounted(native,2)};Check(BackgroundCopies::Start({rename},one,true),"start external journal rename");Until([]{return !BackgroundCopies::Active();});Check(!BackgroundCopies::LastResult().code,"external background rename failed");Until([&]{return Row(one->GetBrowserSession(),L"Updated")>=0&&Row(two->GetBrowserSession(),L"Updated")>=0;});Check(one->IsWindowEnabled()&&two->IsWindowEnabled(),"paired journal operation disabled windows");
  fprintf(report,"PASS: normal paired-journal browser mounting in two windows, secondary-file coordination without blocking unrelated paths, 64-bit timestamp display, 1970/2106 Windows and HFS+ roundtrip, lossless refusal for legacy/native/classic targets, external-journal background rename and cross-window refresh. Hidden synthetic windows; no physical media or computer control.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s; last job: %s\n",error.what(),BackgroundCopies::LastResult().message.c_str());result=1;}
 BackgroundCopies::CancelAll();FileOperations::CancelPending();Until([]{return !BackgroundCopies::Active();});MetadataDialogs::Shutdown();ShutdownStorageUi();BackgroundCopies::enabled=false;while(!app.m_browsers.empty())app.m_browsers.back()->SendMessage(WM_CLOSE);fclose(report);return result;
}
#endif
