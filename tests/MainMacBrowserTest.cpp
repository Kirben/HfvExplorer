// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "MacBrowserModel.h"
#include "MainMacBrowser.h"
#include "StorageJobDialog.h"
#include "HfsPlusDisk.h"
#include "FindFiles.h"
#include "BrowserPresentation.h"
#include "MainMfsFixture.h"
#include "HfsPlusCd.h"
#include "CdImage.h"
#include "DeviceWait.h"
#include <cstdio>
#include <set>
extern "C" {
#include "hfs/ClassicHfs.h"
}
namespace {
using namespace MacBrowser;
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
template<class F> void Reject(F action,const char* message){try{action();}catch(const std::exception&){return;}throw HfsPlus::Error(message);}
std::wstring W(const std::string& p){return {p.begin(),p.end()};}
std::shared_ptr<Volume> Formatted(const std::wstring& path,HfsPlus::Volume::FormatKind kind){
    {auto source=HfsPlus::CreateImage(path.c_str(),8*1024*1024);HfsPlus::Volume::Format(*source,u"Unified",true,kind);}
    return MacBrowser::OpenImage(path);
}
Entry File(const std::u16string& name){Entry e;e.name=name;e.forks[0].size=8193;e.forks[1].size=7777;memcpy(e.finder.data(),"TEXTTEST",8);return e;}
Reader Payload(){return [](unsigned f,uint64_t at,void* p,size_t n){auto b=(uint8_t*)p;for(size_t i=0;i<n;++i)b[i]=uint8_t((at+i)*17+f*71);};}
void Verify(Volume& v,uint32_t id){auto e=v.Stat(id);Check(e.forks[0].size==8193&&e.forks[1].size==7777,"fork lengths survived transfer");
    for(unsigned f=0;f<2;++f){std::vector<uint8_t> actual(size_t(e.forks[f].size)),expected(actual.size());Payload()(f,0,expected.data(),expected.size());v.Read(id,f,0,actual.data(),actual.size());Check(actual==expected,"fork bytes survived transfer");}}
uint32_t Find(Volume& v,uint32_t parent,const std::u16string& name){for(auto& e:v.List(parent))if(e.name==name)return e.id;throw HfsPlus::Error("missing fixture entry");}
class MemoryCd : public MediaImageSource {
public:
 std::vector<uint8_t> bytes;DWORD generation=7;bool workerOnly=false,wrongThread=false;unsigned delay=0;
 explicit MemoryCd(const std::string& path){CFile f;Check(f.Open(path.c_str(),CFile::modeRead|CFile::shareDenyNone)!=FALSE,"read CD fixture");bytes.resize(size_t(f.GetLength()));Check(f.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"load CD fixture");}
 bool Open(int drive,std::string&)override{return drive==3;}
 unsigned long long Size()const override{return bytes.size();}
 bool Verify(DWORD& out)override{if(workerOnly&&GetCurrentThreadId()==StorageUi::state().thread)wrongThread=true;if(delay)Sleep(delay);out=generation;return true;}
 bool Read(unsigned long long at,DWORD n,void* p,DWORD& got)override{
  if(workerOnly&&GetCurrentThreadId()==StorageUi::state().thread)wrongThread=true;got=0;if(at>bytes.size()||n>bytes.size()-at)return false;
  memcpy(p,bytes.data()+size_t(at),n);got=n;return true;}
 DWORD Error()const override{return 0;}
};
class LargeDisplay : public Volume {
public:
 std::u16string Name()const override{return u"Large display fixture";}
 const char* Kind()const override{return "Test";}bool Writable()const override{return false;}bool CaseSensitive()const override{return true;}uint64_t FreeBytes()const override{return 0;}
 Entry Stat(uint32_t id)override{Entry e;e.id=id;e.parent=id==2?1:2;e.name=id==2?Name():u"Large forks";e.directory=id==2;if(id!=2){e.forks[0].size=5ULL*1024*1024*1024;e.forks[1].size=3ULL*1024*1024*1024;}return e;}
 std::vector<Entry> List(uint32_t id)override{Check(id==2,"large display folder");return {Stat(0xfffffff0)};}
 void Read(uint32_t,unsigned,uint64_t,void*,size_t)override{throw HfsPlus::Error("Display must not read file contents");}
 uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only fixture");}
 uint32_t Put(uint32_t,const Entry&,Reader,bool)override{throw HfsPlus::Error("Read-only fixture");}
 void Rename(uint32_t,uint32_t,const std::u16string&)override{throw HfsPlus::Error("Read-only fixture");}
 void Remove(uint32_t)override{throw HfsPlus::Error("Read-only fixture");}
};
struct Tree : HFVExplorerTreeView {void Init(){Initialize();enable_selchange(1);}void PostNcDestroy()override{};};
struct List : CHFVExplorerListView {void PostNcDestroy()override{};};
std::wstring Text(CListCtrl& list,int row){wchar_t text[1024]{};LVITEMW item{};item.iSubItem=0;item.pszText=text;item.cchTextMax=ARRAYSIZE(text);
    SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));return text;}
}

bool TestAutomaticCdSelection(std::function<std::unique_ptr<HfsPlus::Source>(int)> open,
                              CWnd *owner,int expected) {
    unsigned opens=0;std::shared_ptr<MacBrowser::Volume> mounted;
    auto factory=[&](int drive){++opens;auto source=open(drive);
        if(!HfsPlus::Volume::Recognizes(*source))return std::shared_ptr<MacBrowser::Volume>();
        try{return MacBrowser::FromHfsPlus(std::move(source),false);}
        catch(const std::exception& e){throw MacBrowser::DetectedVolumeError(e.what());}};
    auto cd=[](const char*)->UINT{return DRIVE_CDROM;};
    for(const char* path:{"D:\\folder","image.dsk",""})
        if(MacBrowser::OpenCdRoot(path,cd,factory))return false;
    if(MacBrowser::OpenCdRoot("D:\\",[](const char*)->UINT{return DRIVE_FIXED;},factory)||opens)return false;
    bool detected=false;auto result=ShowStorageWaitJob([&](Storage::Job&){
        try{mounted=MacBrowser::OpenCdRoot("d:",cd,factory);}
        catch(const MacBrowser::DetectedVolumeError&){detected=true;throw;}return Storage::Result{};
    },true);
    int actual=result.code?(detected?-1:0):(mounted?1:0);
    return actual==expected&&opens==1&&owner->IsWindowEnabled()&&
        (!mounted||!mounted->Writable());
}
bool TestHfsPlusMainCdBrowser(std::unique_ptr<HfsPlus::Source> source,CWnd* owner,
                             std::function<void()> changeMedia) {
    auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());Tree tree;List list;
    app.m_mac_window_mode=0;
    BrowserSession session;session.Attach(owner,&tree,&list);auto& browser=session.MacBrowser();bool valid=false;
    try {
        CMenu menu;Check(menu.LoadMenu(IDR_MAINFRAME)&&menu.GetMenuState(32902,MF_BYCOMMAND)==UINT(-1),"obsolete CD menu returned");
        Check(tree.Create(nullptr,nullptr,WS_CHILD|TVS_HASLINES,CRect(0,0,200,400),owner,100,nullptr)!=FALSE,"create CD tree");
        Check(list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,700,400),owner,101,nullptr)!=FALSE,"create CD list");
        tree.Init();list.Initialize();std::shared_ptr<MacBrowser::Volume> volume;
        auto mounted=ShowStorageWaitJob([&](Storage::Job&){volume=MacBrowser::FromHfsPlus(std::move(source),false);return Storage::Result{};},true);
        Check(!mounted.code,"mount CD for main browser");int index=browser.AttachTestVolume(volume,"cd-main-test");
        Check(browser.ShowTestFolder(index,2)&&!browser.Writable()&&owner->IsWindowEnabled(),"read-only main CD controls");
        changeMedia();valid=!browser.ShowTestFolder(index,2)&&!session.ListingAvailable()&&!browser.CanExport()&&owner->IsWindowEnabled();
    }catch(const std::exception&){valid=false;}
    browser.Clear();if(list.m_hWnd)list.DestroyWindow();if(tree.m_hWnd)tree.DestroyWindow();
    return valid;
}

int RunMainMacBrowserTest(const char* directory){
    if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;FILE* report=fopen((root+"\\main-mac-browser-report.txt").c_str(),"w");if(!report)return 2;
    try{
        auto source=Formatted(W(root+"\\source.dsk"),HfsPlus::Volume::FormatKind::HfsPlus);
        auto target=Formatted(W(root+"\\target.dsk"),HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        const std::u16string unicode=u"\u65e5\u672c\u8a9e \u03b1 "+std::u16string(80,u'x');
        auto dated=File(unicode);dated.created=2700000000;dated.modified=2800000000;dated.datesValid=true;
        uint32_t folder=source->MakeDirectory(2,u"Folder"),file=source->Put(folder,dated,Payload(),false);
        Verify(*source,file);
        Storage::Job copy;Check(Copy(*source,folder,*target,2,copy),"recursive cross-format copy");
        auto copied=target->Stat(Find(*target,2,u"Folder"));auto children=target->List(copied.id);Check(children.size()==1&&children[0].name==unicode,"long Unicode names survived transfer");Verify(*target,children[0].id);Check(children[0].created==dated.created&&children[0].modified==dated.modified,"HFS+ catalog timestamps preserved");
        auto progress=copy.progress();Check(progress.bytes==15970&&progress.totalBytes==15970&&progress.files==1,"whole-folder both-fork progress");
        Reject([&]{Storage::Job job;Copy(*target,copied.id,*target,copied.id,job);},"self-folder copy was accepted");
        uint32_t nested=target->MakeDirectory(copied.id,u"Nested");Reject([&]{Storage::Job job;Copy(*target,copied.id,*target,nested,job);},"descendant copy was accepted");
        target->Rename(children[0].id,copied.id,u"Renamed");Verify(*target,children[0].id);
        Reject([&]{target->Put(copied.id,File(u"Renamed"),[](unsigned,uint64_t,void*,size_t){throw HfsPlus::Error("injected source failure");},true);},"failed replacement reported success");Verify(*target,children[0].id);
        Storage::Job cancelled;cancelled.cancel();Reject([&]{Copy(*source,folder,*target,nested,cancelled);},"cancelled transfer proceeded");
        std::string host=root+"\\windows";Check(CreateDirectoryA(host.c_str(),nullptr)!=FALSE,"create Windows transfer folder");auto windows=OpenWindows(W(host));
        Storage::Job exportJob;Check(Copy(*source,file,*windows,2,exportJob),"Windows AppleDouble export");auto exported=windows->List();Check(exported.size()==1&&exported[0].name==unicode,"resource companion hidden and Unicode name preserved");Verify(*windows,exported[0].id);Check(windows->Stat(exported[0].id).created==dated.created&&windows->Stat(exported[0].id).modified==dated.modified,"Windows timestamps preserved");Check(windows->Stat(exported[0].id).finder==source->Stat(file).finder,"Finder information survived AppleDouble export");
        Storage::Job importJob;Check(Copy(*windows,exported[0].id,*target,nested,importJob),"Windows AppleDouble import");Verify(*target,target->List(nested)[0].id);
        auto old=windows->List()[0];Reject([&]{windows->Put(2,File(unicode),[](unsigned,uint64_t,void*,size_t){throw HfsPlus::Error("injected input failure");},true);},"Windows replacement error ignored");Verify(*windows,old.id);
        std::string classicPath=root+"\\classic.dsk";{CFile f;Check(f.Open(classicPath.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic image");f.SetLength(4*1024*1024);}
        char label[]="Classic";Check(hfs_format(const_cast<char*>(classicPath.c_str()),0,label)==0,"format classic transfer fixture");
        auto classic=OpenClassic(classicPath,"Classic:",true);uint32_t shortFile=source->Put(2,File(u"Short"),Payload(),false);
        auto copyToClassic=[&](uint32_t id,Storage::CopyPrompt expected,Storage::ConflictChoice answer){
            auto job=Storage::copyWorker().submit([&](Storage::Job& work){
                Check(Copy(*source,id,*classic,2,work),"classic copy incomplete");
                return Storage::Result{};
            });
            bool sawPrompt=false;
            const auto start=GetTickCount64();
            while(!job->done()){
                const auto prompt=job->conflictState();
                if(!prompt.path.empty()){
                    if(prompt.kind!=expected){job->cancel();job->wait();Check(false,"unexpected classic copy prompt");}
                    sawPrompt=true;
                    job->resolve(prompt.id,answer);
                }
                if(GetTickCount64()-start>15000){job->cancel();job->wait();Check(false,"classic copy timed out");}
                Sleep(1);
            }
            Check(sawPrompt,"classic copy omitted its preflight decision");
            return job->result();
        };
        Check(!copyToClassic(shortFile,Storage::CopyPrompt::ClassicMetadataLoss,Storage::ConflictChoice::Replace).code,"HFS+ to classic HFS");Verify(*classic,classic->List()[0].id);
        Storage::Job fromClassic;Check(Copy(*classic,classic->List()[0].id,*target,2,fromClassic),"classic HFS to HFSX");Verify(*target,Find(*target,2,u"Short"));
        Check(copyToClassic(file,Storage::CopyPrompt::ClassicArchiveNames,Storage::ConflictChoice::Cancel).error==ERROR_CANCELLED&&classic->List().size()==1,"declined archive name preview changed classic destination");
        auto windowsAgain=OpenWindows(W(host));Reject([&]{Storage::Job job;Copy(*windows,exported[0].id,*windowsAgain,2,job,true);},"same Windows destination accepted across adapters");Verify(*windows,exported[0].id);
        auto classicAgain=OpenClassic(classicPath,"Classic:",true);Reject([&]{Storage::Job job;Copy(*classic,classic->List()[0].id,*classicAgain,2,job,true);},"same classic destination accepted across adapters");
        MainMfsFixture::Fixture mfsFixture;std::string mfsPath=root+"\\mfs.dsk";
        {CFile f;Check(f.Open(mfsPath.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"write MFS browser fixture");f.Write(mfsFixture.bytes.data(),UINT(mfsFixture.bytes.size()));}
        auto mfs=MacBrowser::OpenImage(W(mfsPath));Check(mfs&&std::string(mfs->Kind())=="MFS"&&mfs->Supports().create&&!mfs->Supports().createFolders,"MFS writable flat capabilities");
        auto mfsEntries=mfs->List();Check(mfsEntries.size()==4&&mfsEntries[2].name.size()==255&&mfsEntries[2].name.back()==0xe9,"MFS long MacRoman names converted without shortening");
        Reject([&]{Storage::Job job;RemoveTree(*mfs,mfsEntries[0].id,job);},"MFS deletion was enabled");
        Storage::Job mfsCopy;Check(Copy(*mfs,mfsEntries[0].id,*target,2,mfsCopy),"MFS both-fork copy into shared writable adapter");
        auto mfsCopied=target->Stat(Find(*target,2,u"Read Me"));Check(mfsCopied.forks[0].size==2536&&mfsCopied.forks[1].size==1177,"MFS fork lengths retained");
        for(unsigned fork=0;fork<2;++fork){std::vector<uint8_t> a(size_t(mfsCopied.forks[fork].size)),b(a.size());mfs->Read(mfsEntries[0].id,fork,0,a.data(),a.size());target->Read(mfsCopied.id,fork,0,b.data(),b.size());Check(a==b,"MFS fork bytes retained");}
        FindRequest request;request.scope.origin=FileOperations::Location::Mounted(source);request.scope.catalogRoute={2};
        request.pattern=BrowserPresentation::Utf8(u"\u65e5\u672c");auto results=std::make_shared<FindResults>();Storage::Job search;
        auto searchResult=MakeFindFilesJob(request,results)(search);
        Check(searchResult.code==0&&results->utf8&&results->hits.size()==1&&results->hits[0].nativePath==std::vector<uint32_t>({2,folder,file}),"Unicode search and ID navigation routes");
        Check(FindNameMatches(std::wstring(L"\u0391lpha"),std::wstring(L"\u03b1*"),false),"Unicode wildcard case folding");
        uint32_t batchFolder=target->MakeDirectory(2,u"Batch");Storage::Job batch;
        Check(CopyMany({{source,file},{source,shortFile}},*target,batchFolder,batch)&&batch.progress().totalFiles==2&&batch.progress().files==2&&batch.progress().bytes==31940&&batch.progress().totalBytes==31940,"whole-selection copy totals do not restart for each file");
        auto sensitive=target->CaseSensitive();Check(sensitive,"HFSX case-sensitive adapter");
        auto insensitive=Formatted(W(root+"\\insensitive.dsk"),HfsPlus::Volume::FormatKind::HfsXCaseInsensitive);
        Check(!insensitive->CaseSensitive()&&std::string(insensitive->Kind())=="HFSX","case-insensitive HFSX adapter");
        fprintf(report,"PASS: MFS flat listing/copy and locked-file protection with 255-byte MacRoman names; shared Unicode search routes; self-copy guards across separate Windows/classic adapters.\n");
        fprintf(report,"PASS: shared adapters preserve long Unicode names, both forks and Finder metadata across HFS+/HFSX, Windows AppleDouble and classic HFS; recursive totals, self/descendant rejection, cancellation and failed-replacement preservation.\n");
        auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());CWnd owner;Check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden unified browser",WS_OVERLAPPED,0,0,700,400,nullptr,0)!=FALSE,"create hidden owner");
        app.m_pMainWnd=&owner;app.m_mac_window_mode=0;InitializeStorageUi();
        Tree tree;List list;BrowserSession session;session.Attach(&owner,&tree,&list);
        Check(tree.Create(nullptr,nullptr,WS_CHILD|TVS_HASLINES,CRect(0,0,200,400),&owner,100,nullptr)!=FALSE,"create real tree view");
        Check(list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,700,400),&owner,101,nullptr)!=FALSE,"create real list view");tree.Init();list.Initialize();
        storageUiTest={};auto failed=ShowStorageWaitJob([&](Storage::Job&){
            source->Put(2,File(u"Slow failed import"),[](unsigned,uint64_t,void*,size_t){Sleep(250);throw HfsPlus::Error("injected slow read failure");},false);
            return Storage::Result{};
        },true);
        Check(failed.code&&storageUiTest.timerTicks>0&&source->Writable()&&owner.IsWindowEnabled()&&app.m_pMainWnd==&owner,"slow failed import preserves responsive writable adapter and owner");
        Reject([&]{Find(*source,2,u"Slow failed import");},"failed import left a catalog entry");
        auto& browser=session.MacBrowser();int index=browser.AttachTestVolume(source,"source");Check(browser.ShowTestFolder(index,2),"show source root in main panes");
        auto& lc=list.GetListCtrl();int folderRow=-1;for(int i=0;i<lc.GetItemCount();++i)if(Text(lc,i)==L"Folder")folderRow=i;
        Check(folderRow>=0&&browser.Activate(folderRow)&&session.AwaitNavigationForTest(),"main list folder activation");bool found=false;for(int i=0;i<lc.GetItemCount();++i)found|=Text(lc,i)==Wide(unicode);
        Check(found&&browser.Active()&&browser.Writable(),"main list retained full Unicode filename and writable state");
        Check(browser.Refresh()&&session.AwaitNavigationForTest()&&owner.IsWindowEnabled(),"main browser refresh and owner restoration");
        app.m_show_invisible_mac=1;int mfsIndex=browser.AttachTestVolume(mfs,"mfs");Check(browser.ShowTestFolder(mfsIndex,2)&&browser.Writable()&&!browser.CanCreateFolder(),"MFS uses the main panes with writable flat actions");
        bool longMfs=false;for(int row=0;row<lc.GetItemCount();++row)longMfs|=Text(lc,row).size()==255;Check(longMfs,"long MFS name in main list");
        // Exercise the actual existing Copy/Paste and Delete command entry points.
        Check(browser.ShowTestFolder(index,2),"return to source root");int copyRow=-1;
        for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Short")copyRow=row;
        Check(copyRow>=0,"source clipboard row");lc.SetItemState(-1,0,LVIS_SELECTED);lc.SetItemState(copyRow,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);list.edit_copy();
        uint32_t clipboardFolder=target->MakeDirectory(2,u"Clipboard");int clipboardIndex=browser.AttachTestVolume(target,"clipboard-target");Check(browser.ShowTestFolder(clipboardIndex,2),"target root");
        int folderRow2=-1;for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Clipboard")folderRow2=row;
        Check(folderRow2>=0&&browser.Activate(folderRow2)&&session.AwaitNavigationForTest(),"target folder navigation");storageUiTest={};list.edit_paste();session.AwaitNavigationForTest();
        Verify(*target,Find(*target,clipboardFolder,u"Short"));Check(owner.IsWindowEnabled(),"copy command restored main owner");
        for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Short"){lc.SetItemState(-1,0,LVIS_SELECTED);lc.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);break;}
        list.try_delete_selected_items(FALSE);Reject([&]{Find(*target,clipboardFolder,u"Short");},"Delete command did not remove selected native file");Verify(*source,shortFile);
        uint32_t moved=source->Put(2,File(u"Move me"),Payload(),false);Check(browser.ShowTestFolder(index,2),"source move root");
        for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Move me"){lc.SetItemState(-1,0,LVIS_SELECTED);lc.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);break;}
        list.edit_cut();Check(browser.ShowTestFolder(clipboardIndex,2),"move target root");
        for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Clipboard"){Check(browser.Activate(row)&&session.AwaitNavigationForTest(),"open Clipboard target");break;}
        list.edit_paste();session.AwaitNavigationForTest();Reject([&]{source->Stat(moved);},"cut source was not removed after committed copy");Verify(*target,Find(*target,clipboardFolder,u"Move me"));
        fprintf(report,"PASS: existing main-list Copy, Paste and Delete commands use the shared adapter and restore the owner.\n");
        target->Put(2,File(u"Readme"),Payload(),false);target->Put(2,File(u"README"),Payload(),false);
        int xIndex=browser.AttachTestVolume(target,"hfsx");Check(browser.ShowTestFolder(xIndex,2)&&browser.Writable(),"HFSX in main panes");
        bool mixed=false,upper=false;for(int row=0;row<lc.GetItemCount();++row){mixed|=Text(lc,row)==L"Readme";upper|=Text(lc,row)==L"README";}
        Check(mixed&&upper,"main browser retains case-distinct HFSX entries");
        int ciIndex=browser.AttachTestVolume(insensitive,"hfsx-ci");Check(browser.ShowTestFolder(ciIndex,2)&&browser.Writable(),"case-insensitive HFSX in main panes");
        auto large=std::make_shared<LargeDisplay>();int largeIndex=browser.AttachTestVolume(large,"large");Check(browser.ShowTestFolder(largeIndex,2)&&lc.GetItemCount()==1,"large-fork display");
        wchar_t sizeText[100]{};LVITEMW sizeItem{};sizeItem.iSubItem=FIELD_SIZE;sizeItem.pszText=sizeText;sizeItem.cchTextMax=ARRAYSIZE(sizeText);SendMessageW(lc.m_hWnd,LVM_GETITEMTEXTW,0,reinterpret_cast<LPARAM>(&sizeItem));
        auto rowData=list.BrowserRowAt(0);Check(std::wstring(sizeText)==L"8388608 KB"&&rowData&&uint32_t(rowData->entry.id)==0xfffffff0&&!browser.SelectionWritable()&&browser.CanExport(),"64-bit displayed fork sizes and unsigned catalog IDs");
        auto media=std::make_shared<MemoryCd>(root+"\\target.dsk");auto unchanged=media->bytes;auto cd=FromHfsPlus(HfsPlus::OpenCd(3,media),false);
        media->workerOnly=true;media->delay=90;storageUiTest={};int cdIndex=browser.AttachTestVolume(cd,"cd");
        Check(browser.ShowTestFolder(cdIndex,2)&&!browser.Writable()&&owner.IsWindowEnabled()&&!media->wrongThread,"slow CD metadata stays on worker and restores main owner");
        ++media->generation;Check(!browser.ShowTestFolder(cdIndex,2)&&!session.ListingAvailable()&&!browser.CanExport()&&owner.IsWindowEnabled()&&media->bytes==unchanged,"changed CD invalidates main list without writes");
        fprintf(report,"PASS: both HFSX comparison modes, 64-bit fork-size presentation, full-width catalog IDs, slow read-only CD metadata, media-change invalidation and owner restoration in main panes.\n");
        auto featurePath=W(root+"\\..\\feature-links.dsk");
        if(GetFileAttributesW(featurePath.c_str())!=INVALID_FILE_ATTRIBUTES){
            auto feature=MacBrowser::OpenImage(featurePath);int featureIndex=browser.AttachTestVolume(feature,"links");
            Check(browser.ShowTestFolder(featureIndex,2),"show link fixture");int linkRow=-1;
            for(int row=0;row<lc.GetItemCount();++row)if(Text(lc,row)==L"Linked folder")linkRow=row;
            Check(linkRow>=0&&browser.Activate(linkRow)&&session.AwaitNavigationForTest(),"open directory hard link in main browser");bool child=false;
            for(int row=0;row<lc.GetItemCount();++row)child|=Text(lc,row)==L"Child";
            Check(child&&!browser.Writable(),"linked folder contents and mutation restrictions");
            auto selected=tree.GetTreeCtrl().GetSelectedItem();auto parent=tree.GetTreeCtrl().GetParentItem(selected);
            tree.GetTreeCtrl().SelectItem(parent);browser.Select(tree.GetTreeCtrl().GetItemData(parent));Check(session.AwaitNavigationForTest(),"return from directory hard link");
            bool returned=false;for(int row=0;row<lc.GetItemCount();++row)returned|=Text(lc,row)==L"Linked folder";
            Check(returned,"directory hard-link navigation returns to visible parent");
            fprintf(report,"PASS: main-browser directory hard-link navigation returns to visible parent.\n");
        }
        browser.Clear();list.DestroyWindow();tree.DestroyWindow();owner.DestroyWindow();app.m_pMainWnd=nullptr;
        fprintf(report,"PASS: real main tree/list controls show Unicode names, open folders, refresh and restore the owner; no standalone browser or physical media used.\n");
    }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());fclose(report);return 1;}catch(CException* e){char message[512];e->GetErrorMessage(message,sizeof(message));e->Delete();fprintf(report,"FAIL: %s\n",message);fclose(report);return 1;}
    fclose(report);return 0;
}
#endif
