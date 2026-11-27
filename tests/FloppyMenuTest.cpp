// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "FloppyImage.h"
#include "HfsPlus.h"
#include "ClassicFormatCommand.h"
#include "VolumeCoordinator.h"
#include "StorageJobDialog.h"
#include "resource.h"
#include "OptionsPage1.h"
#include "AskNewVolume.h"
#include "AskVolumeToFloppy.h"
#include "FloppyWriteDialog.h"
#include "ReadOnlyCdBackend.h"
#include "FloppyWriter.h"
#include "FakeFloppy.h"
extern "C" {
#undef ERROR
#include "hfs/ClassicHfsEngine.h"
#include "hfs/interface.h"
#define _FHOOK_INTERNAL_
#include "Filehook.h"
}
#include <stdexcept>
namespace {
ftype_write originalWrite;
bool failUnmountWrite=false,failCleanMarker=false;
int unmountWrite(int fd,const void* bytes,unsigned count) {
 if(failUnmountWrite||(failCleanMarker&&count==512&&((const unsigned char*)bytes)[0]==0x42&&((const unsigned char*)bytes)[1]==0x44&&(((const unsigned char*)bytes)[10]&1))){failUnmountWrite=false;failCleanMarker=false;errno=EIO;hfs_error=const_cast<char*>("Injected unmount write failure");return -1;}
 return originalWrite(fd,bytes,count);
}
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
class NewVolumeDialogTest : public CAskNewVolume {
 UINT DriveKind(int drive) override{return drive==0||drive==25?DRIVE_REMOVABLE:drive==2?DRIVE_FIXED:DRIVE_NO_ROOT_DIR;}
};
class OriginalFloppyDialogTest : public CAskVolumeToFloppy {
 UINT DriveKind(int drive) override{return drive==1||drive==25?DRIVE_REMOVABLE:DRIVE_NO_ROOT_DIR;}
};
class DialogWriterTest : public FloppyWriteApi {
public:
 FakeFloppy disk;unsigned writes=0;bool locked=false,corrupt=false;DWORD openError=0;unsigned delay=0;
 DialogWriterTest():disk(std::vector<BYTE>(1474560)){}
 UINT DriveType(int d) override{return disk.DriveType(d);}
 HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f) override{disk.openError=openError;return disk.Open(p,a,s,f);}
 bool Geometry(HANDLE h,DISK_GEOMETRY& g) override{return disk.Geometry(h,g);}
 bool Verify(HANDLE h,DWORD& c) override{return disk.Verify(h,c);}
 bool Read(HANDLE h,ULONGLONG o,DWORD n,void* p,DWORD& got) override{if(delay)Sleep(delay);bool ok=disk.Read(h,o,n,p,got);if(ok&&corrupt)static_cast<BYTE*>(p)[0]^=1;return ok;}
 void Close(HANDLE h) override{disk.Close(h);}
 bool Lock(HANDLE) override{locked=true;return true;}
 bool Dismount(HANDLE) override{return locked;}
 bool Unlock(HANDLE) override{locked=false;return true;}
 bool Write(HANDLE,ULONGLONG o,DWORD n,const void* p,DWORD& got) override{if(!locked||o>disk.media.size()||n>disk.media.size()-o)return false;memcpy(disk.media.data()+size_t(o),p,n);got=n;++writes;return true;}
 bool Flush(HANDLE) override{SetLastError(ERROR_INVALID_FUNCTION);return false;}
};
}
int RunFloppyMenuTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;
 std::string root=directory;FILE* report=nullptr;fopen_s(&report,(root+"\\floppy-menu-report.txt").c_str(),"w");if(!report)return 2;
 try {
  const std::string preserved=root+"\\existing.dsk",fresh=root+"\\fresh.dsk";
  {CFile f;require(f.Open(preserved.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create format preservation fixture");f.Write("DO NOT FORMAT",13);f.Close();}
  char label[]="SafeFormat",bad[]="Invalid:Name";
  readonly_cd_backend_init();
  const auto rejects=[](const std::string& path,const char* name,hfv_size_t size){
   try{FormatClassicImage(VolumeCoord::Wide(path),name,size);}
   catch(const HfsPlus::Error&){return true;}
   return false;
  };
  for(char* name : {bad,label}) {
   require(rejects(preserved,name,1474560),"new-volume format replaced an existing file");
   CFile f;require(f.Open(preserved.c_str(),CFile::modeRead)!=FALSE&&f.GetLength()==13,"format changed existing image length");char bytes[13];require(f.Read(bytes,13)==13&&!memcmp(bytes,"DO NOT FORMAT",13),"format changed existing bytes");f.Close();
  }
  require(rejects(fresh,bad,1474560)&&GetFileAttributesA(fresh.c_str())==INVALID_FILE_ATTRIBUTES,"invalid format name created an image");
  require(rejects(fresh,label,12345)&&GetFileAttributesA(fresh.c_str())==INVALID_FILE_ATTRIBUTES,"invalid format size created an image");
  FormatClassicImage(VolumeCoord::Wide(fresh),label,1474560);
  readonly_cd_backend_final();
  fprintf(report,"PASS: new classic HFS formatting validates names/sizes before creation and preserves existing files.\n");
  for(int failure : {0,1,2,3}) {
   std::string path=root+"\\unmount"+std::to_string(failure)+".dsk";
   {CFile f;require(f.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"create unmount fixture");f.SetLength(4*1024*1024);f.Close();}
   require(hfs_format(const_cast<char*>(path.c_str()),0,label)==0,"format unmount fixture");
   hfsvol* volume=hfs_mount(const_cast<char*>(path.c_str()),0,O_RDWR);require(volume!=nullptr,"mount unmount fixture");
   char entry[]="SafeFormat:Pending",type[]="TEXT",creator[]="TEST";
   require(hfs_create(volume,entry,type,creator)==0,"create pending entry");hfsfile* file=hfs_open(volume,entry);require(file!=nullptr&&hfs_write(file,const_cast<char*>("PENDING"),7)==7,"write open file");
   if(failure==1)file->parid=0x7ffffff0; // Force a file-catalog close failure.
   originalWrite=fhook_write;if(failure==2||failure==3){failUnmountWrite=failure==2;failCleanMarker=failure==3;fhook_write=unmountWrite;}
   int result=hfs_umount(volume);fhook_write=originalWrite;
   require((result!=0)==(failure!=0),"unmount suppressed a pending metadata/write error");
   CFile f;require(f.Open(path.c_str(),CFile::modeRead)!=FALSE,"read unmount marker");f.Seek(1024+10,CFile::begin);unsigned char flags[2];require(f.Read(flags,2)==2,"read HFS attributes");f.Close();
   require(((flags[0]&1)!=0)==(failure==0),"failed unmount published a clean HFS marker");
  }
  fprintf(report,"PASS: unmount reports file-close/cache-write failures and keeps failed volumes dirty; normal unmount remains clean.\n");
  FakeFloppy sourceDisk(std::vector<BYTE>(1474560));
  auto sameDrive=Storage::worker().submit(MakeFloppyImageJob(0,"A:\\unsafe.dsk",&sourceDisk));sameDrive->wait();
  require(sameDrive->result().code!=0&&sourceDisk.opens==0,"source drive accepted as image destination");
  for(unsigned sectors:{9u,18u,36u}){
   std::vector<BYTE> bytes(80*2*sectors*512);for(size_t i=0;i<bytes.size();++i)bytes[i]=BYTE(i*37+i/512);
   FakeFloppy api(bytes);api.geometry.SectorsPerTrack=sectors;api.geometry.MediaType=sectors==9?F3_720_512:sectors==18?F3_1Pt44_512:F3_2Pt88_512;
   auto path=root+"\\disk"+std::to_string(sectors)+".dsk";
   auto job=Storage::worker().submit(MakeFloppyImageJob(0,path,&api));job->wait();
   require(job->result().code==0,"raw floppy image read failed");
   CFile file;require(file.Open(path.c_str(),CFile::modeRead)!=FALSE,"saved image missing");std::vector<BYTE> actual(size_t(file.GetLength()));file.Read(actual.data(),UINT(actual.size()));file.Close();
   require(actual==bytes&&api.readOnly&&api.aligned&&api.media==bytes,"raw byte fidelity and read-only aligned access");
   require(job->progress().processed==bytes.size(),"image progress incomplete");
  }
  for(int mode=0;mode<4;++mode){
   FakeFloppy api(std::vector<BYTE>(1474560));if(mode==0)api.readError=ERROR_CRC;if(mode==1)api.shortRead=true;if(mode==2)api.changeAfterRead=1;if(mode==3)api.geometry.SectorsPerTrack=17;
   auto path=root+"\\bad"+std::to_string(mode)+".dsk";auto job=Storage::worker().submit(MakeFloppyImageJob(0,path,&api));job->wait();
   require(job->result().code!=0&&GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES,"failed read published an image");
  }
  CWnd owner;require(owner.CreateEx(0,AfxRegisterWndClass(0),"Hidden floppy options test",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"hidden owner");
  COptionsPage1 page;require(page.Create(IDD_OPTIONS_PAGE_1,&owner)!=FALSE,"General page");
  require(page.GetDlgItem(IDC_OPTIONS_STARTUP_FLOPPIES)->IsWindowEnabled()!=FALSE,"startup floppy checkbox disabled");
  require(page.GetDlgItem(1001)==nullptr,"obsolete CD patch control remains");
  page.m_startup_floppies=TRUE;page.UpdateData(FALSE);require(page.IsDlgButtonChecked(IDC_OPTIONS_STARTUP_FLOPPIES)==BST_CHECKED,"startup checkbox binding");
  page.CheckDlgButton(IDC_OPTIONS_STARTUP_FLOPPIES,BST_UNCHECKED);require(page.UpdateData(TRUE)&&!page.m_startup_floppies,"startup checkbox roundtrip");
  page.DestroyWindow();
  NewVolumeDialogTest format;format.m_allowFloppies=true;format.m_volsize_ex="10 MB";
  require(format.Create(IDD_ASK_NEW_VOLUME,&owner)!=FALSE,"new-volume dialog");
  CString caption;
  format.GetWindowText(caption);
  require(caption=="Format New Volume"&&!(format.GetDlgItem(IDC_NEW_VOLUME_FLOPPY_WARNING)->GetStyle()&WS_VISIBLE),
          "format caption or initial floppy warning");
  auto paths=(CComboBox*)format.GetDlgItem(IDC_VOLPATH);
  int a=paths->FindStringExact(-1,CAskNewVolume::FloppyLabel(0));
  require(a!=CB_ERR&&paths->FindStringExact(-1,CAskNewVolume::FloppyLabel(25))!=CB_ERR&&paths->FindStringExact(-1,CAskNewVolume::FloppyLabel(2))==CB_ERR,"new-volume drive discovery and fixed-drive filtering");
  paths->SetCurSel(a);format.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_VOLPATH,CBN_SELCHANGE),(LPARAM)paths->GetSafeHwnd());
  CString size;format.GetDlgItemText(IDC_VOLSIZE_EX,size);
  require(size=="1440 kB"&&!format.GetDlgItem(IDC_VOLSIZE_EX)->IsWindowEnabled()&&!format.GetDlgItem(IDC_VOLNAME_BROWSE)->IsWindowEnabled(),"floppy size fixed to 1.44 MB and image browse disabled");
  require((format.GetDlgItem(IDC_NEW_VOLUME_FLOPPY_WARNING)->GetStyle()&WS_VISIBLE)!=0,
          "physical format erase warning missing");
  paths->SetCurSel(-1);paths->SetWindowText("C:\\test.dsk");format.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_VOLPATH,CBN_EDITCHANGE),(LPARAM)paths->GetSafeHwnd());
  require(format.GetDlgItem(IDC_VOLSIZE_EX)->IsWindowEnabled()&&format.GetDlgItem(IDC_VOLNAME_BROWSE)->IsWindowEnabled(),"image controls restored after floppy selection");
  require(!(format.GetDlgItem(IDC_NEW_VOLUME_FLOPPY_WARNING)->GetStyle()&WS_VISIBLE),
          "floppy erase warning remained when formatting an image");
  format.DestroyWindow();
  NewVolumeDialogTest image;image.m_volpath="C:\\existing.dsk";
  require(image.Create(IDD_ASK_NEW_VOLUME,&owner)!=FALSE,"existing-image dialog");
  require(((CComboBox*)image.GetDlgItem(IDC_VOLPATH))->FindStringExact(-1,CAskNewVolume::FloppyLabel(0))==CB_ERR,"existing-image initialization must not offer a physical target");
  image.DestroyWindow();
  for(bool write:{false,true}){
   OriginalFloppyDialogTest dialog;dialog.to_floppy=write;dialog.m_caption=write?"Write a volume file to floppy":"Create a volume file from floppy";
   require(dialog.Create(IDD_VOLUME_TO_FLOPPY,&owner)!=FALSE,"original File floppy dialog");
   dialog.GetWindowText(caption);
   wchar_t action[20]{};
   GetDlgItemTextW(dialog.m_hWnd,IDOK,action,ARRAYSIZE(action));
   require(caption==(write?"Write Image to Floppy":"Create a volume file from floppy")&&
           std::wstring(action)==(write?L"Write\u2026":L"Save")&&
           bool(dialog.GetDlgItem(IDC_FLOPPY_WRITE_WARNING)->GetStyle()&WS_VISIBLE)==write,
           "write/read dialog title, action or erase warning");
   auto drives=(CComboBox*)dialog.GetDlgItem(IDC_DUMP_TO_FLOPPY);
   require(drives->GetCount()==2&&int(drives->GetItemData(0))==1&&int(drives->GetItemData(1))==25,"original drive combo stores actual B/Z letters instead of indices");
   drives->SetCurSel(1);dialog.SetDlgItemText(IDC_VOLPATH,"C:\\test.dsk");require(dialog.UpdateData(TRUE)&&dialog.m_drive==25,"original dialog drive selection binding");
   require(dialog.GetDlgItem(1214)==nullptr,"replacement low-level formatting checkbox remains");dialog.DestroyWindow();
  }
  require(CAskNewVolume::FloppySelection("<Format floppy Z:>")==25&&CAskNewVolume::FloppySelection("C:\\image.dsk")==-1,"unambiguous floppy selection parsing");
  CMenu menu;require(menu.LoadMenu(IDR_MAINFRAME)!=FALSE,"main menu resource");
  require(menu.GetMenuState(32810,MF_BYCOMMAND)==0xffffffff&&menu.GetMenuState(ID_FILE_NEWVOLUME,MF_BYCOMMAND)!=0xffffffff,"duplicate Tools formatter absent and File formatter retained");
  std::vector<BYTE> formatted;std::string why;require(PrepareFloppyHfs("DialogTest",formatted,why),"prepare original dialog format image");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();storageUiTest={};
#ifdef HFV_FLOPPY_TESTING
  {
   readonly_cd_backend_init();
   FakeFloppy media(formatted);
   media.changeCount=41;
   require(readonly_floppy_attach_api(25,&media),"install simulated preparation drive");
   struct Detach {~Detach(){readonly_floppy_attach_api(25,nullptr);readonly_cd_backend_final();}} detach;
   const auto path=root+"\\prepared-source.dsk";
   {CFile file;require(file.Open(path.c_str(),CFile::modeWrite|CFile::modeCreate),"write prep source");
    file.Write(formatted.data(),UINT(formatted.size()));}
   auto prepared=std::make_shared<PreparedFloppyImage>();
   auto work=MakeFloppyPreparationJob(25,ExistingFloppyImage{path},prepared);
   {
    VolumeCoord::Lease occupied({{VolumeCoord::DeviceKey(25),VolumeCoord::Mode::Write}});
    auto busy=Storage::worker().submit(work);
    busy->wait();
    require(busy->result().error==ERROR_BUSY&&media.opens==0&&prepared->bytes.empty(),"busy preparation opened the floppy");
   }
   auto loaded=Storage::worker().submit(work);
   loaded->wait();
   require(!loaded->result().code&&prepared->bytes==formatted&&prepared->generation==41&&
           media.readOnly&&media.opens==media.closes&&media.media==formatted,
           "preparation did not stage source bytes and capture the floppy generation read-only");
   auto generated=std::make_shared<PreparedFloppyImage>();
   auto format=Storage::worker().submit(MakeFloppyPreparationJob(25,NewFloppyImage{"Background floppy"},generated));
   format->wait();
   require(!format->result().code&&ValidateFloppyHfs(generated->bytes)&&generated->generation==41&&media.media==formatted,
           "generated floppy image touched physical media before confirmation");
   fprintf(report,"PASS: floppy preparation validates copied or generated images, captures media generation and leaves writes for confirmation.\n");
  }
#endif
  DialogWriterTest writer;writer.delay=1;
  auto written=ShowStorageJob(MakeFloppyWriteJob(1,formatted,&writer),"Hidden original floppy writer",true,true,false);
  require(written.code==0&&writer.disk.media==formatted&&writer.writes==160&&!writer.locked&&writer.disk.opens==writer.disk.closes,"format/write worker verifies the full image and accepts an unsupported flush");
  require(storageUiTest.copyBarCount==1&&storageUiTest.timerTicks>=2&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"original-dialog write uses responsive single progress bar and restores owner");
  DialogWriterTest denied;denied.openError=ERROR_ACCESS_DENIED;
  auto failed=Storage::worker().submit(MakeFloppyWriteJob(1,formatted,&denied));failed->wait();
  require(failed->result().error==ERROR_ACCESS_DENIED&&denied.writes==0,"writer access error before writes");
  DialogWriterTest badReadback;badReadback.corrupt=true;
  auto corrupted=Storage::worker().submit(MakeFloppyWriteJob(1,formatted,&badReadback));corrupted->wait();
  require(corrupted->result().code!=0&&corrupted->result().message.find("incomplete")!=std::string::npos&&!badReadback.locked,"write readback error warns disk incomplete and releases lock");
  AfxGetApp()->m_pMainWnd=nullptr;
  owner.DestroyWindow();
  WIN32_FIND_DATAA found;HANDLE h=FindFirstFileA((root+"\\hcd*.tmp").c_str(),&found);if(h!=INVALID_HANDLE_VALUE)FindClose(h);require(h==INVALID_HANDLE_VALUE,"partial temporary image leaked");
  fprintf(report,"PASS: simulated raw floppy image reads, errors/media changes and cleanup; General option; new-volume discovery, image isolation and drive binding; original read/write dialog layout and B/Z drive selection; native format/write worker with full readback, unsupported flush, errors, responsive one-bar progress and owner restoration; duplicate Tools item absent. No physical drives accessed.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
