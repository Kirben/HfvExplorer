// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "FindFiles.h"
#include "VolumeCoordinator.h"
#include "MacBrowserModel.h"
#include "StorageJobDialog.h"
#include "FloppyWriter.h"
#include "ReadOnlyCdBackend.h"
#include "FakeFloppy.h"
#include "floppy.h"
#include <winioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdexcept>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
extern bool ProbeFindResultsDialog(CWnd*,const FindResults&,bool);
namespace {
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void save(const std::string& path,const std::vector<BYTE>& bytes){CFile file;check(file.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create search fixture");file.Write(bytes.data(),UINT(bytes.size()));}
std::vector<BYTE> load(const std::string& path){CFile file;check(file.Open(path.c_str(),CFile::modeRead)!=FALSE,"open fixture");std::vector<BYTE> bytes(size_t(file.GetLength()));check(file.Read(bytes.data(),UINT(bytes.size()))==bytes.size(),"read fixture");return bytes;}
class SlowSearchFloppy : public FakeFloppy {
public:
 using FakeFloppy::FakeFloppy;
 bool Read(HANDLE h,ULONGLONG offset,DWORD count,void* bytes,DWORD& got) override{Sleep(80);return FakeFloppy::Read(h,offset,count,bytes,got);}
};
}
int RunFindFilesTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;
 FILE* report=nullptr;if(fopen_s(&report,(root+"\\find-report.txt").c_str(),"w")||!report)return 2;
 try{
  check(FindNameMatches("ReadMe","read")&&!FindNameMatches("ReadMe","read",true),"case-insensitive substring and optional case matching");
  check(FindNameMatches("README.TXT","read*.t?t")&&FindNameMatches("NoExtension","*.*")&&FindNameMatches("README","*")&&!FindNameMatches("README","*.txt")&&!FindNameMatches("README",""),"wildcards, question mark, extensionless files and empty patterns");
  auto execute=[&](FindRequest request){auto results=std::make_shared<FindResults>();auto job=Storage::worker().submit(MakeFindFilesJob(request,results));job->wait();return std::make_pair(job,results);};
  std::vector<BYTE> blank;std::string error;check(PrepareFloppyHfs("FindMac",blank,error),"prepare HFS search image");
  std::string image=root+"\\find.dsk";save(image,blank);std::vector<char> imagePath(image.c_str(),image.c_str()+image.size()+1);
  hfsvol* volume=hfs_mount(imagePath.data(),0,O_RDWR);check(volume!=nullptr,"mount search fixture for setup");
  char folder[]="FindMac:Folder",deep[]="FindMac:Folder:Deep";check(hfs_mkdir(volume,folder)==0&&hfs_mkdir(volume,deep)==0,"create nested HFS folders");
  char type[]="TEXT",creator[]="TEST";
  for(const char* name:{"FindMac:Root.txt","FindMac:Folder:ReadMe","FindMac:Folder:Deep:readme.txt"}){
   std::vector<char> path(name,name+strlen(name)+1);check(hfs_create(volume,path.data(),type,creator)==0,"create HFS filename");hfsfile* file=hfs_open(volume,path.data());check(file!=nullptr,"open HFS fixture");
   check(hfs_write(file,"HELLO",5)==5&&hfs_setfork(file,1)==0&&hfs_write(file,"RSRC",4)==4&&hfs_close(file)==0,"write both fixture forks");
  }
  check(hfs_umount(volume)==0,"finish HFS fixture");auto original=load(image);
  FindRequest mac;mac.scope.origin=FileOperations::Location::Classic(image,"FindMac:");mac.scope.volumeRoot=mac.scope.origin;mac.pattern="*.txt";
  auto recursive=execute(mac);check(recursive.first->result().code==0&&recursive.second->hits.size()==2&&recursive.second->hits[0].bytes==9,"recursive HFS names, both-fork size and sorted matches");
  mac.scope.origin=FileOperations::Location::Classic(image,"FindMac:Folder");mac.pattern="read";check(execute(mac).second->hits.size()==2,"selected HFS folder scope");
  mac.wholeVolume=true;mac.pattern="Root.txt";
  check(execute(mac).second->hits.size()==1,"whole-volume search used the selected nested folder");
  mac.wholeVolume=false;mac.pattern="read";
  mac.recursive=false;check(execute(mac).second->hits.size()==1,"nonrecursive HFS search");
  mac.recursive=true;mac.caseSensitive=true;check(execute(mac).second->hits.size()==1,"case-sensitive HFS search");
  mac.caseSensitive=false;mac.pattern="Deep";mac.includeFolders=true;auto folders=execute(mac);check(folders.second->hits.size()==1&&folders.second->hits[0].folder&&folders.second->hits[0].parent=="FindMac:Folder","folder-name search and containing-folder path");
  mac.scope.origin=FileOperations::Location::Classic(image,"FindMac:");mac.pattern="*";mac.includeFolders=false;mac.limit=1;auto capped=execute(mac);check(capped.first->result().code==0&&capped.second->hits.size()==1&&capped.second->truncated,"result limit is bounded and reported");
  mac.limit=10000;mac.pattern="";check(execute(mac).first->result().error==ERROR_INVALID_PARAMETER,"empty pattern rejected");
  mac.pattern="*";mac.scope.origin=FileOperations::Location::Classic(image,"FindMac:Missing");check(execute(mac).first->result().code!=0,"missing HFS search folder reported");
  check(load(image)==original,"HFS searches never modify source image");
  std::string host=root+"\\host",nested=host+"\\Nested";check(CreateDirectoryA(host.c_str(),nullptr)&&CreateDirectoryA(nested.c_str(),nullptr),"create Windows folders");
  save(host+"\\ReadMe",{'R'});save(nested+"\\readme.txt",{'T'});
  std::string large=host+"\\Large.bin";HANDLE big=CreateFileA(large.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);DWORD ignored=0;LARGE_INTEGER size;size.QuadPart=0x100000025ULL;
  check(big!=INVALID_HANDLE_VALUE&&DeviceIoControl(big,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&ignored,nullptr)&&SetFilePointerEx(big,size,nullptr,FILE_BEGIN)&&SetEndOfFile(big),"create sparse >4-GiB metadata fixture");CloseHandle(big);
  FindRequest windows;windows.scope.origin=FileOperations::Location::Windows(VolumeCoord::Wide(host));windows.pattern="read";
  auto win=execute(windows);check(win.first->result().code==0&&win.second->hits.size()==2,"recursive Windows name search");
  windows.recursive=false;check(execute(windows).second->hits.size()==1,"nonrecursive Windows name search");
  windows.recursive=true;windows.pattern="Large";auto bigResult=execute(windows);check(bigResult.second->hits.size()==1&&bigResult.second->hits[0].bytes==0x100000025ULL,"Windows size retains high 32 bits without reading file contents");
  std::string loop=host+"\\Loop";bool link=CreateSymbolicLinkA(loop.c_str(),host.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)!=FALSE;
  if(link){windows.pattern="*.*";auto linked=execute(windows);check(linked.first->result().code==0&&linked.second->hits.size()==3,"directory reparse points not traversed");check(RemoveDirectoryA(loop.c_str())!=FALSE,"remove test link only");}
  else fprintf(report,"NOTE: directory-link test skipped because link creation is unavailable (Windows %lu).\n",GetLastError());
  windows.pattern="*";windows.limit=1;
  auto cappedWindows=execute(windows);
  check(cappedWindows.first->result().code==0&&cappedWindows.second->hits.size()==1&&
        cappedWindows.second->truncated&&cappedWindows.second->hits.front().reveal.IsWindows(),
        "common Windows match limit and typed location");
  {
   auto mounted=MacBrowser::OpenClassic(image,"FindMac:",false);
   FindRequest scoped;scoped.scope.origin=FileOperations::Location::Mounted(mounted);
   scoped.pattern="read";scoped.limit=1;
   auto cappedMounted=execute(scoped);
   check(cappedMounted.first->result().code==0&&cappedMounted.second->hits.size()==1&&
         cappedMounted.second->truncated&&cappedMounted.second->hits.front().reveal.IsMounted()&&
         cappedMounted.second->hits.front().nativePath.size()>1,
         "common mounted match limit and typed catalog route");
   mounted->Close();
  }
  windows.scope.origin=FileOperations::Location::Windows(VolumeCoord::Wide(root+"\\Missing"));check(execute(windows).first->result().code!=0,"missing Windows search folder reported");
  readonly_cd_backend_init();char physical[64];get_floppy_volume_file_name(25,physical);FakeFloppy fake(original);check(readonly_floppy_attach_api(25,&fake),"attach read-only search floppy");
  mac.scope.origin=FileOperations::Location::Classic(physical,"FindMac:");mac.pattern="read";auto physicalResult=execute(mac);
  check(physicalResult.first->result().code==0&&physicalResult.second->hits.size()==2&&fake.readOnly&&fake.media==original&&fake.opens==fake.closes,"native floppy adapter search is read-only and closes handles");
  fake.readError=ERROR_NOT_READY;check(execute(mac).first->result().code!=0,"device read errors reported without false successful empty result");fake.readError=0;
  check(readonly_floppy_attach_api(25,nullptr),"detach search floppy");
  CWnd owner;CString ownerClass=AfxRegisterWndClass(0);check(owner.CreateEx(0,ownerClass,"Hidden find owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"create hidden find owner");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();
  check(ProbeFindResultsDialog(&owner,*recursive.second,true)&&ProbeFindResultsDialog(&owner,*bigResult.second,false)&&ProbeFindResultsDialog(&owner,FindResults{},false),"virtual results rows display names and 64-bit sizes, selection and empty results");
  SlowSearchFloppy slow(original);check(readonly_floppy_attach_api(25,&slow),"attach slow search media");storageUiTest={};auto uiResults=std::make_shared<FindResults>();
  auto ui=ShowStorageJob(MakeFindFilesJob(mac,uiResults),"Hidden slow find",true,true,true);
  check(ui.code==0&&uiResults->hits.size()==2&&storageUiTest.timerTicks>=2&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"slow search stays responsive and restores main window");
  storageUiTest={};storageUiTest.cancelOnTimer=true;auto cancelled=ShowStorageJob(MakeFindFilesJob(mac,std::make_shared<FindResults>()),"Hidden cancelled find",true,true,true);
  check(cancelled.error==ERROR_CANCELLED&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled()&&slow.media==original,"Cancel stops search without writes and restores owner");
  check(readonly_floppy_attach_api(25,nullptr),"detach slow search media");owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(report,"PASS: case/substring/wildcard matching, HFS/Windows recursion and folder scope, folder names, both-fork/64-bit sizes, result limits, read/path errors, read-only physical adapter, virtual results display/selection, slow reads, cancellation and pre-close owner restoration; no physical devices.\n");
 }catch(const std::exception& error){fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}catch(CException* error){char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: %s\n",text);error->Delete();fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
