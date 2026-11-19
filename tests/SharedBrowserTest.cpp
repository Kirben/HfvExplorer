// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "MainMacBrowser.h"
#include "StorageJobDialog.h"
#include "TransferFixture.h"
#include "DeviceWait.h"
#include "utils.h"
#include <cstdio>
#include <fcntl.h>
#include <winioctl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}
namespace {
void check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
struct Doc : CHFVExplorerDoc {Doc():CHFVExplorerDoc(false){};};
struct Tree : HFVExplorerTreeView {
 void Init(){Initialize();enable_selchange(0);}
 void Bind(CDocument* doc){m_pDocument=doc;}
 HTREEITEM Hfs(){return initialize_volume(0,"Shared",2,0,0,0,0);}
 HTREEITEM Windows(const CString& path){CString label="Windows",copy=path;return initialize_fat_volume(0,&label,DRIVE_FIXED,&copy,FALSE);}
 void PostNcDestroy()override{}
};
struct List : CHFVExplorerListView {int RenameForTest(int row,char* from,char* to){return do_rename(row,from,to,1);}void DeleteForTest(){try_delete_selected_items(FALSE);}void Bind(CDocument* doc){m_pDocument=doc;}void PostNcDestroy()override{};};
std::wstring text(CListCtrl& list,int row){wchar_t name[1024]{};LVITEMW value{};value.pszText=name;value.cchTextMax=ARRAYSIZE(name);
 SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&value));return name;}
int find(CListCtrl& list,const wchar_t* name){for(int row=0;row<list.GetItemCount();++row)if(text(list,row)==name)return row;return -1;}
std::vector<BYTE> load(const CString& path){CFile file;check(file.Open(path,CFile::modeRead|CFile::shareDenyNone)!=FALSE,"open snapshot fixture");std::vector<BYTE> bytes(size_t(file.GetLength()));check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"read snapshot fixture");return bytes;}
void host(const CString& path,uint64_t size,uint64_t date){HANDLE h=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);check(h!=INVALID_HANDLE_VALUE,"create Windows fixture");DWORD n;DeviceIoControl(h,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&n,nullptr);LARGE_INTEGER end;end.QuadPart=size;check(SetFilePointerEx(h,end,nullptr,FILE_BEGIN)&&SetEndOfFile(h),"size sparse Windows fixture");auto time=BrowserDirectory::FileTime(date);check(SetFileTime(h,&time,nullptr,&time)!=FALSE,"set exact file timestamps");CloseHandle(h);}
}
int RunSharedBrowserTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;CString root=directory;FILE* report=fopen((root+"\\shared-browser-report.txt"),"w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());CWnd owner;app.m_startup_floppies=app.m_startup_cds=app.m_startup_hds=0;
 app.m_show_invisible_mac=app.m_show_invisible_fat=1;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=0;
 Doc doc;Tree tree;List list;
 try{
  check(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden shared browser",WS_OVERLAPPED,0,0,700,400,nullptr,0)!=FALSE,"create hidden main owner");
  app.m_pMainWnd=&owner;InitializeStorageUi();
  check(tree.Create(nullptr,nullptr,WS_CHILD|TVS_HASLINES,CRect(0,0,200,400),&owner,100,nullptr)!=FALSE,"create tree");
  check(list.Create(nullptr,nullptr,WS_CHILD|LVS_REPORT,CRect(200,0,700,400),&owner,101,nullptr)!=FALSE,"create list");
  doc.GetBrowserSession().Attach(&owner,&tree,&list);tree.Init();list.Initialize();tree.Bind(&doc);list.Bind(&doc);
  CString image=root+"\\classic.dsk";{CFile file;check(file.Open(image,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create classic image");file.SetLength(4*1024*1024);}
  char volumeName[]="Shared",type[]="TEXT",creator[]="TEST";
  check(hfs_format(image.GetBuffer(),0,volumeName)==0,"format classic fixture");image.ReleaseBuffer();
  auto volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount classic fixture");
  check(hfs_mkdir(volume,const_cast<char*>("Shared:Folder"))==0,"create classic folder");
  for(auto item:{std::make_pair("Shared:A larger",uint32_t(2600)),std::make_pair("Shared:Z smaller",uint32_t(2200))}){
   check(hfs_create(volume,const_cast<char*>(item.first),type,creator)==0,"create classic file");auto file=hfs_open(volume,const_cast<char*>(item.first));check(file!=nullptr,"open classic file");std::vector<BYTE> bytes(item.second,17);
   check(hfs_write(file,bytes.data(),ULONG(bytes.size()))==bytes.size()&&hfs_close(file)==0,"write classic file");
  }
  check(hfs_umount(volume)==0,"close classic fixture");auto original=load(image);
  doc.m_hfs_count=1;doc.m_volumes[0].m_file_name=image;doc.m_volumes[0].m_volume_name="Shared";
  auto opened=ShowStorageWaitJob([&](Storage::Job&){check(doc.m_volumes[0].open()!=0,"open classic browser volume");return Storage::Result{};},true);check(!opened.code,"classic volume read job");
  doc.m_volumes[0].m_rootitem=tree.Hfs();tree.GetTreeCtrl().SelectItem(doc.m_volumes[0].m_rootitem);
  bool wrongThread=false;DWORD ui=GetCurrentThreadId();BrowserDirectory::testObserveRead=[&](const char*){wrongThread|=GetCurrentThreadId()==ui;Sleep(80);};storageUiTest={};
  doc.open_sub_directory(0,2,"Shared");BrowserDirectory::testObserveRead={};
  auto& control=list.GetListCtrl();check(control.GetItemCount()==3&&!wrongThread&&storageUiTest.timerTicks>0&&owner.IsWindowEnabled(),"slow classic snapshot stays on worker and restores owner");
  int larger=find(control,L"A larger"),smaller=find(control,L"Z smaller");check(larger>=0&&smaller>=0,"classic rows in shared renderer");
  auto model=list.BrowserRowAt(larger);check(model&&model->storageType==LIST_HFS&&model->entry.forks[0].size==2600&&model->entry.finder[0]=='T',"classic normalized bytes/type/binding");
  CString source;check(list.get_path(larger,source)&&source=="Shared:A larger","classic conversion path binding");
  auto classicItem = doc.GetBrowserSession().MacBrowser().CaptureItem(larger);
  check(classicItem.IsClassic() && classicItem.ClassicPath()==(LPCSTR)source &&
        classicItem.expectedCatalogId==model->id, "classic command did not use the published browser row");
  list.SortBrowserRows(FIELD_SIZE,true);check(text(control,0)==L"Folder"&&text(control,1)==L"Z smaller","exact classic byte sorting within identical displayed KB");
  check(load(image)==original,"browsing classic image modified source bytes");
  // The first HFS partition is numbered one; raw images remain supported too.
  auto partitioned=std::vector<BYTE>(65536+original.size(),0);
  memcpy(partitioned.data()+65536,original.data(),original.size());
  auto word=[](BYTE* p,unsigned n){p[0]=BYTE(n>>8);p[1]=BYTE(n);};
  auto number=[](BYTE* p,unsigned n){p[0]=BYTE(n>>24);p[1]=BYTE(n>>16);p[2]=BYTE(n>>8);p[3]=BYTE(n);};
  word(partitioned.data(),0x4552);word(partitioned.data()+2,512);number(partitioned.data()+4,unsigned(partitioned.size()/512));
  BYTE* map=partitioned.data()+512;word(map,0x504d);number(map+4,1);number(map+8,128);number(map+12,unsigned(original.size()/512));memcpy(map+48,"Apple_HFS",9);
  CString mapped=root+"\\partitioned.dsk";{CFile file;check(file.Open(mapped,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create partitioned fixture");file.Write(partitioned.data(),UINT(partitioned.size()));}
  doc.m_volumes[0].close();doc.m_volumes[0].m_file_name=mapped;
  auto mappedJob=ShowStorageWaitJob([&](Storage::Job&){BrowserFolderReader reader({},&doc.GetBrowserSession().Mounts());auto rows=reader.read_classic_browser_directory(0,2,"Shared:",false,0);check(rows.rows.size()==3,"partitioned directory row count");return Storage::Result{};},true);
  check(!mappedJob.code&&load(mapped)==partitioned,"partitioned classic listing and unchanged source");
  {auto adapter=MacBrowser::OpenClassic((LPCSTR)mapped,"Shared:",false);check(adapter->List(2).size()==3,"partitioned classic transfer adapter");adapter->Close();}
  doc.m_volumes[0].close();doc.m_volumes[0].m_file_name=image;check(doc.m_volumes[0].open()!=0,"restore raw classic fixture");
  doc.m_volumes[0].m_is_cd=1;doc.open_sub_directory(0,2,"Shared");check(!list.BrowserDirectoryWritable(),"classic read-only backend capabilities lost");
  doc.m_volumes[0].m_is_cd=0;
  unsigned calls=0;BrowserDirectory::testObserveRead=[&](const char*){if(++calls==3)throw std::runtime_error("injected classic read error");};
  doc.open_sub_directory(0,2,"Shared");BrowserDirectory::testObserveRead={};check(control.GetItemCount()==0&&!list.BrowserDirectoryWritable()&&owner.IsWindowEnabled(),"failed classic snapshot published partial rows");
  doc.open_sub_directory(0,2,"Shared");check(control.GetItemCount()==3,"classic listing retry");
  CString windows=root+"\\windows";check(CreateDirectoryA(windows,nullptr)!=FALSE,"create Windows directory");
  uint64_t date=BrowserDirectory::MacTime(3000000000UL);host(windows+"\\A newer",2600,date+2);host(windows+"\\Z older",2200,date+1);
  host(windows+"\\Large",6ULL*1024*1024*1024,date);check(CreateDirectoryA(windows+"\\Folder",nullptr)!=FALSE,"create Windows folder");
  {auto target=MacBrowser::OpenWindows(std::wstring((LPCSTR)windows,(LPCSTR)windows+windows.GetLength()));MacBrowser::Entry e;e.name=u"Forked";e.forks[0].size=17;e.forks[1].size=32;memcpy(e.finder.data(),"TEXTTEST",8);
   target->Put(2,e,[](unsigned,uint64_t,void* data,size_t n){memset(data,0,n);},false);target->Close();}
  doc.m_fat_count=1;doc.m_fats[0].m_letter=windows[0];doc.m_fats[0].m_type=DRIVE_FIXED;doc.m_fats[0].m_rootitem=tree.Windows(windows);tree.GetTreeCtrl().SelectItem(doc.m_fats[0].m_rootitem);
  BrowserDirectory::testObserveRead=[&](const char*){wrongThread|=GetCurrentThreadId()==ui;Sleep(80);};storageUiTest={};
  app.m_pMainWnd=nullptr;
  doc.open_fat_directory(0,windows.GetBuffer(),1);windows.ReleaseBuffer();BrowserDirectory::testObserveRead={};
  app.m_pMainWnd=&owner;
  check(control.GetItemCount()==6&&!wrongThread&&storageUiTest.timerTicks>0&&owner.IsWindowEnabled(),"Windows snapshot worker and parent entry");
  int large=find(control,L"Large");check(large>=0&&list.BrowserRowAt(large)->entry.forks[0].size==6ULL*1024*1024*1024,"Windows backend lost high file-size bits");
  int forked=find(control,L"Forked");check(forked>=0&&list.BrowserRowAt(forked)->entry.forks[0].size==17&&list.BrowserRowAt(forked)->entry.forks[1].size==32,"Windows companion metadata does not use shared model");
  list.SortBrowserRows(FIELD_MODIFIED,true);check(text(control,1)==L"Folder"&&text(control,3)==L"Z older"&&text(control,4)==L"A newer","Windows FILETIME precision sorting");
  CString path;int row=find(control,L"A newer");check(list.get_path(row,path)&&path==windows+"\\A newer","Windows backend operation path binding");
  auto windowsItem = doc.GetBrowserSession().MacBrowser().CaptureItem(row);
  check(windowsItem.IsWindows() && windowsItem.WindowsPath()==VolumeCoord::Wide(std::string((LPCSTR)path)) &&
        windowsItem.expectedIdentity==list.BrowserRowAt(row)->entry.identity &&
        !windowsItem.expectedIdentity.empty(), "Windows command lost the published file identity");
  control.SetItemText(row,0,"Unrelated label");
  CString capturedPath;
  check(list.get_path(row,capturedPath)&&capturedPath==path&&
        doc.GetBrowserSession().MacBrowser().CaptureItem(row).expectedIdentity==windowsItem.expectedIdentity,
        "a control label redirected a typed file command");
  const auto folderRow=list.BrowserRowAt(find(control,L"Folder"));
  check(folderRow&&folderRow->treeItem,"missing Windows child folder in tree");
  auto folderTarget=doc.GetBrowserSession().MacBrowser().TargetFolder(folderRow->treeItem);
  check(folderTarget.IsWindows()&&folderTarget.WindowsPath()==folderRow->location.WindowsPath()&&
        folderTarget.expectedIdentity==folderRow->location.expectedIdentity&&!folderTarget.expectedIdentity.empty(),
        "tree command lost the published Windows destination identity");
  calls=0;BrowserDirectory::testObserveRead=[&](const char*){if(++calls==3)throw std::runtime_error("injected Windows read error");};
  doc.open_fat_directory(0,windows.GetBuffer(),1);windows.ReleaseBuffer();BrowserDirectory::testObserveRead={};
  check(control.GetItemCount()==1&&list.BrowserRowAt(0)==nullptr&&!list.BrowserDirectoryWritable()&&owner.IsWindowEnabled(),"failed Windows snapshot published partial rows");
  doc.open_fat_directory(0,windows.GetBuffer(),1);windows.ReleaseBuffer();check(control.GetItemCount()==6,"Windows listing retry");
  {int selected=find(control,L"A newer");char from[]="A newer",to[]="Job renamed";
   check(selected>=0&&list.RenameForTest(selected,from,to),"Windows rename view did not submit a job");
   doc.open_fat_directory(0,windows.GetBuffer(),1);windows.ReleaseBuffer();selected=find(control,L"Job renamed");check(selected>=0,"Windows rename listing refresh");
   control.SetItemState(-1,0,LVIS_SELECTED);control.SetItemState(selected,LVIS_SELECTED,LVIS_SELECTED);list.DeleteForTest();doc.GetBrowserSession().AwaitNavigationForTest();check(GetFileAttributesA(windows+"\\Job renamed")==INVALID_FILE_ATTRIBUTES&&owner.IsWindowEnabled(),"Windows deletion view did not complete its independent job");
  }
  // Exercise the retained conversion backend using a path produced by the common view.
  auto copied=TransferFixture::Submit((LPCSTR)image,(LPCSTR)source,(LPCSTR)windows,(LPCSTR)(windows+"\\typed.bin"),0,0,'5');
  copied->wait();check(!copied->result().code,copied->result().message.c_str());check(GetFileAttributesA(windows+"\\typed.bin")!=INVALID_FILE_ATTRIBUTES,"classic MacBinary conversion route was lost");
  auto imported=TransferFixture::Submit((LPCSTR)windows,(LPCSTR)(windows+"\\typed.bin"),(LPCSTR)image,"Shared:Imported",0,0,'a');
  imported->wait();check(!imported->result().code,"classic Automatic import route was lost");
  doc.open_sub_directory(0,2,"Shared");check(find(control,L"Imported")>=0,"classic catalog refresh after converted import");
  {int selected=find(control,L"Imported");char from[]="Imported",to[]="Job imported";check(selected>=0&&list.RenameForTest(selected,from,to),"classic HFS rename view job");
   doc.open_sub_directory(0,2,"Shared");selected=find(control,L"Job imported");check(selected>=0,"classic HFS rename listing");control.SetItemState(-1,0,LVIS_SELECTED);control.SetItemState(selected,LVIS_SELECTED,LVIS_SELECTED);list.DeleteForTest();
   doc.open_sub_directory(0,2,"Shared");check(control.GetItemCount()==3&&owner.IsWindowEnabled(),"classic HFS deletion view job");
  }
  BrowserDirectory::Row a,b;a.entry.name=u"A";b.entry.name=u"Z";a.entry.forks[0].size=UINT64_MAX;b.entry.forks[0].size=UINT64_MAX-1;
  check(BrowserDirectory::Compare(a,b,FIELD_SIZE,true)>0&&BrowserDirectory::Bytes(a.entry)==UINT64_MAX,"64-bit typed sort overflow");
  a.entry.name=u"README";b.entry.name=u"Readme";check(BrowserDirectory::Compare(a,b,FIELD_NAME,true)!=0,"case-distinct deterministic ordering");
  fprintf(report,"PASS: shared classic HFS/Windows/native row model, exact byte/FILETIME sorting, 6 GiB host files, classic Finder metadata and operation paths, read-only classic browsing with byte-for-byte unchanged source, slow worker reads, atomic error publication, retry and owner restoration.\n");
  fprintf(report,"PASS: classic BinHex 5/MacBinary I export and Automatic import remain in the existing conversion backend. No physical devices or computer control used.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());BrowserDirectory::testObserveRead={};fclose(report);return 1;}
 BrowserDirectory::testObserveRead={};doc.m_volumes[0].close();tree.Bind(nullptr);list.Bind(nullptr);
 list.DestroyWindow();tree.DestroyWindow();owner.DestroyWindow();app.m_pMainWnd=nullptr;
 fclose(report);return 0;
}
#endif
