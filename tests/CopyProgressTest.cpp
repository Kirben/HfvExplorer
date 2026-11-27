// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include <stdio.h>
#include <fcntl.h>
#include <vector>
#include "TransferFixture.h"
#include "VolumeCoordinator.h"
#include "resource.h"
#include "DynCopyModeSelect.h"
#include "OptionsPage2.h"
#include "HostFileSizes.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "FloppyWriter.h"
#include "DirectFloppy.h"
#define _FHOOK_INTERNAL_
#include "Filehook.h"
#include "ReadOnlyCdBackend.h"
#include "FakeFloppy.h"
#include "Mactypes.h"
#include "floppy.h"
#include <thread>
#include <winioctl.h>
#include <stdexcept>
extern "C" {
#include "hfs/ClassicHfs.h"
#include "hfs/interface.h"
extern int hfv_replacement_fault;
}
void import_map(CString& path);
extern "C" int tmap_final(void);
namespace {
class CopyFloppy : public FloppyWriteApi {
public:
 FakeFloppy disk;unsigned writes=0,locks=0;bool locked=false;int failure=0;
 explicit CopyFloppy(const std::vector<BYTE>& bytes):disk(bytes){}
 UINT DriveType(int d) override{return disk.DriveType(d);}
 HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f) override {
  if((a&GENERIC_WRITE)&&failure==1){SetLastError(ERROR_WRITE_PROTECT);return INVALID_HANDLE_VALUE;}
  return disk.Open(p,a,s,f);
 }
 bool Geometry(HANDLE h,DISK_GEOMETRY& g) override{return disk.Geometry(h,g);}
 bool Verify(HANDLE h,DWORD& n) override{return disk.Verify(h,n);}
 bool Read(HANDLE h,ULONGLONG o,DWORD n,void* p,DWORD& got) override {
  bool ok=disk.Read(h,o,n,p,got);
  if(failure==4){SetLastError(ERROR_CRC);return false;}
  return ok;
 }
 void Close(HANDLE h) override{disk.Close(h);}
 bool Lock(HANDLE) override {
  ++locks;if(failure==2){SetLastError(ERROR_ACCESS_DENIED);return false;}
  locked=true;if(failure==5)++disk.changeCount;return true;
 }
 bool Dismount(HANDLE) override{return locked;}
 bool Unlock(HANDLE) override{locked=false;return true;}
 bool Write(HANDLE,ULONGLONG o,DWORD n,const void* p,DWORD& wrote) override {
  if(!locked)throw std::runtime_error("Write without lock");++writes;
  wrote=failure==3?n-1:n;memcpy(disk.media.data()+size_t(o),p,wrote);return true;
 }
 bool Flush(HANDLE) override{SetLastError(failure==6?ERROR_WRITE_FAULT:ERROR_INVALID_FUNCTION);return false;}
};
void check(bool ok,const char* message) {if(!ok)throw message;}
void expectNoConflict(const std::shared_ptr<Storage::Job>& job) {
 const auto start=GetTickCount();
 while(!job->done()){
  if(!job->conflict().empty()||GetTickCount()-start>10000){
   job->cancel();
   job->wait();
   throw std::runtime_error("An invalid or self-copy reached the overwrite stage or stalled");
  }
  Sleep(1);
 }
}
unsigned confirmReplacement(const std::shared_ptr<Storage::Job>& job){
 DWORD deadline=GetTickCount()+15000;unsigned conflicts=0;std::string previous;
 while(!job->done()){
  auto conflict=job->conflict();if(!conflict.empty()&&conflict!=previous){++conflicts;previous=conflict;job->resolve(Storage::ConflictChoice::ReplaceAll);}
  if(LONG(deadline-GetTickCount())<=0){job->cancel();job->wait();throw std::runtime_error("Confirmed replacement timed out");}Sleep(1);
 }
 return conflicts;
}
std::vector<BYTE> load(const CString& path) {
 CFile file;check(file.Open(path,CFile::modeRead|CFile::shareDenyWrite)!=FALSE,"open exported file");
 std::vector<BYTE> bytes((size_t)file.GetLength());check(file.Read(bytes.data(),(UINT)bytes.size())==bytes.size(),"read exported bytes");return bytes;
}
}
int RunCopyProgressTest(const char* directory) {
 if(!CreateDirectoryA(directory,NULL))return 2;
 CString root=directory,image=root+"\\source.dsk",report=root+"\\copy-progress-report.txt";
 FILE* out=nullptr;if(fopen_s(&out,report,"w") || !out)return 2;
 try {
  CFile host;check(host.Open(image,CFile::modeCreate|CFile::modeReadWrite)!=FALSE,"create image");
  host.SetLength(4*1024*1024);host.Close();
  char label[]="CopyTest";check(hfs_format(image.GetBuffer(),0,label)==0,"format fixture");image.ReleaseBuffer();
  hfsvol* volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount fixture");
  char folder[]="CopyTest:Folder",nested[]="CopyTest:Folder:Nested",binary[]="CopyTest:Folder:Nested:Game",text[]="CopyTest:Folder:Text";
  check(hfs_mkdir(volume,folder)==0 && hfs_mkdir(volume,nested)==0,"nested fixture folders");
  char type[]="APPL",creator[]="TEST";check(hfs_create(volume,binary,type,creator)==0,"create binary entry");hfsfile* file=hfs_open(volume,binary);check(file!=nullptr,"create two-fork file");
  std::vector<BYTE> data(16385),resource(2049);for(size_t i=0;i<data.size();++i)data[i]=BYTE(i*37);
  for(size_t i=0;i<resource.size();++i)resource[i]=BYTE(i*71);
  check(hfs_write(file,data.data(),(unsigned long)data.size())==data.size() && hfs_setfork(file,1)==0 &&
   hfs_write(file,resource.data(),(unsigned long)resource.size())==resource.size() && hfs_close(file)==0,"write both forks");
  strcpy_s(type,"TEXT");check(hfs_create(volume,text,type,creator)==0,"create text entry");file=hfs_open(volume,text);check(file!=nullptr,"create text");
  check(hfs_write(file,"HELLO\n",6)==6 && hfs_close(file)==0 && hfs_umount(volume)==0,"finish fixture");
  // An explicit path, case alias and containing folder must never replace the source.
  for(const char* destination : {"CopyTest:Folder:Nested:Game", "copytest:folder:nested:game", "CopyTest:Folder:Nested:"}) {
   for(int move : {0,1}) {
    auto same=TransferFixture::Submit((LPCSTR)image,binary,(LPCSTR)image,destination,0,move,'a');
    expectNoConflict(same);check(same->result().code!=0,"reject copy/move onto the same HFS catalog entry before mutation");
    volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen after self-copy rejection");
    file=hfs_open(volume,binary);check(file!=nullptr,"self-copy preserved source entry");
    std::vector<BYTE> verify(data.size());check(hfs_read(file,verify.data(),(unsigned long)verify.size())==verify.size()&&verify==data,"self-copy preserved data fork");
    verify.resize(resource.size());check(hfs_setfork(file,1)==0&&hfs_read(file,verify.data(),(unsigned long)verify.size())==verify.size()&&verify==resource,"self-copy preserved resource fork");
    check(hfs_close(file)==0&&hfs_umount(volume)==0,"close self-copy check");
   }
  }
  fprintf(out,"PASS: self-copy/self-move rejects exact paths, case aliases and containing folders, preserving both forks.\n");
  auto missingHfs=TransferFixture::Submit((LPCSTR)image,"CopyTest:Missing:Game",(LPCSTR)image,"CopyTest:Folder:Nested:",0,0,'a');
  missingHfs->wait();check(missingHfs->result().code!=0,"missing HFS source fails");
  volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen after failed preflight");
  file=hfs_open(volume,binary);check(file!=nullptr,"failed HFS source preflight preserves existing destination");
  std::vector<BYTE> preservedFork(data.size());check(hfs_read(file,preservedFork.data(),(unsigned long)preservedFork.size())==preservedFork.size()&&preservedFork==data,"failed preflight preserves destination data");
  preservedFork.resize(resource.size());check(hfs_setfork(file,1)==0&&hfs_read(file,preservedFork.data(),(unsigned long)preservedFork.size())==preservedFork.size()&&preservedFork==resource,"failed preflight preserves destination resource fork");
  check(hfs_close(file)==0&&hfs_umount(volume)==0,"close preflight preservation check");
  fprintf(out,"PASS: missing HFS source cannot delete an existing destination during failure cleanup.\n");
  // Replacement needs space for the new forks without freeing the old file first.
  CString replacementInput=root+"\\Game";
  {CFile f;check(f.Open(replacementInput,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create replacementInput replacement");f.SetLength(5*1024*1024);f.Close();}
  HFSIFACE_set_modein(4,0);
  auto fullReplacement=TransferFixture::Submit((LPCSTR)root,(LPCSTR)replacementInput,(LPCSTR)image,"CopyTest:Folder:Nested:",0,1,'r');
  check(confirmReplacement(fullReplacement)==1,"HFS replacement must ask before staging a conflicting file");check(fullReplacement->result().code!=0&&GetFileAttributesA(replacementInput)!=INVALID_FILE_ATTRIBUTES,"disk-full replacement fails and retains move source");
  volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen full destination");
  file=hfs_open(volume,binary);check(file!=nullptr,"disk-full replacement retained previous HFS entry");
  preservedFork.resize(data.size());check(hfs_read(file,preservedFork.data(),(unsigned long)preservedFork.size())==preservedFork.size()&&preservedFork==data,"disk-full preserves previous HFS data fork");
  preservedFork.resize(resource.size());check(hfs_setfork(file,1)==0&&hfs_read(file,preservedFork.data(),(unsigned long)preservedFork.size())==preservedFork.size()&&preservedFork==resource,"disk-full preserves previous HFS resource fork");
  check(hfs_close(file)==0&&hfs_umount(volume)==0,"close full destination");
  DeleteFileA(replacementInput);
  HFSIFACE_set_modein(0,0);
  fprintf(out,"PASS: disk-full HFS replacement preserves the previous entry, both forks and host move source.\n");
  for(const char* destination : {"CopyTest:Folder:","CopyTest:Folder:Nested:"}) {
   auto recursive=TransferFixture::Submit((LPCSTR)image,"CopyTest:Folder",(LPCSTR)image,destination,1,0,'a');
   expectNoConflict(recursive);check(recursive->result().code!=0,"reject recursive HFS self-copy before creating children");
  }
  fprintf(out,"PASS: folder copies into themselves or descendants are rejected before recursive mutation.\n");
  {
   const CString input=root+"\\stale-cut";
   {CFile f;check(f.Open(input,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create stale-destination source");f.Write("KEEP",4);}
   using namespace FileOperations;
   auto source=Location::Windows(VolumeCoord::Wide(input.GetString()));
   source.cut=true;
   auto destination=Location::Classic(image.GetString(),"CopyTest:Folder:Nested:");
   destination.directory=true;
   destination.expectedCatalogId=UINT32_MAX;
   const auto assertReplaced=[&](Request request){
    auto operation=Submit(std::move(request));
    expectNoConflict(operation.Job());
    const auto result=operation.Job()->result();
    check(result.code&&result.message.find("folder was replaced")!=std::string::npos&&
          load(input)==std::vector<BYTE>({'K','E','E','P'}),
          "ordinary and converted Cut must reject a stale destination and retain the source");
   };
   assertReplaced(Request(OrdinaryCopy{{source},destination}));
   LegacyConversion conversion{source,destination,false,true,false,CaptureLegacyCopyOptions('r')};
   conversion.options.input='r';
   conversion.target=ConversionTarget::Folder;
   assertReplaced(Request(std::move(conversion)));
   auto verify=MacBrowser::OpenClassic(image.GetString(),"CopyTest:Folder:Nested:",false);
   MacBrowser::Entry created;
   check(!verify->FindChild(2,u"stale-cut",created),"stale destination accepted file data");
   fprintf(out,"PASS: ordinary and converted Cut reject a stale destination before writing and retain the source.\n");
  }
  auto original=TransferFixture::Submit((LPCSTR)image,"CopyTest:Folder",(LPCSTR)root,(LPCSTR)root,1,0,'a');
  original->wait();
  auto progress=original->progress();
  check(original->result().code==0&&progress.files==2&&progress.totalFiles==2&&
        progress.bytes==progress.processed&&progress.processed==progress.totalBytes,
        "typed recursive Automatic export accounts for published files and byte totals");
  auto macbinary=load(root+"\\Folder\\Nested\\Game.bin");
  check(macbinary.size()>=128+16512+resource.size() && !memcmp(macbinary.data()+128,data.data(),data.size()) &&
   !memcmp(macbinary.data()+128+16512,resource.data(),resource.size()),"MacBinary preserves both forks under progress");
  CString legacyPath=root+"\\binhex5.bin";
  auto legacyJob=TransferFixture::Submit((LPCSTR)image,"CopyTest:Folder:Nested:Game",(LPCSTR)root,(LPCSTR)legacyPath,0,0,'5');
  legacyJob->wait();check(legacyJob->result().code==0,"BinHex 5 worker export");
  auto legacy=load(legacyPath);check(legacy.size()==macbinary.size(),"legacy fork alignment under worker");
  for(unsigned i=99;i<128;++i)check(legacy[i]==0,"worker selected MacBinary I header");
  check(!memcmp(legacy.data()+128,data.data(),data.size()) && !memcmp(legacy.data()+128+16512,resource.data(),resource.size()),"legacy worker export both forks");
  for(int fault : {2,0}) {
   hfv_replacement_fault=fault;
   auto replacement=TransferFixture::Submit((LPCSTR)root,(LPCSTR)legacyPath,(LPCSTR)image,binary,0,0,'a');
   confirmReplacement(replacement);hfv_replacement_fault=0;
   check((replacement->result().code!=0)==(fault!=0),"HFS replacement publication result");
   volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen published/rolled-back replacement");
   file=hfs_open(volume,binary);check(file!=nullptr,"publication rollback preserves original name");
   std::vector<BYTE> bytes(data.size());check(hfs_read(file,bytes.data(),(unsigned long)bytes.size())==bytes.size()&&bytes==data,"HFS replacement/rollback preserves data");
   bytes.resize(resource.size());check(hfs_setfork(file,1)==0&&hfs_read(file,bytes.data(),(unsigned long)bytes.size())==bytes.size()&&bytes==resource,"HFS replacement/rollback preserves resource");
   check(hfs_close(file)==0&&hfs_umount(volume)==0,"close replacement check");
  }
  fprintf(out,"PASS: staged HFS replacement publishes both forks and restores the original after a publication failure.\n");
  CString damagedLegacy=root+"\\damaged.bin";
  {CFile f;check(f.Open(damagedLegacy,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create damaged import");f.Write(legacy.data(),(UINT)legacy.size()-200);f.Close();}
  HFSIFACE_set_modein(0,0);
  auto damagedJob=TransferFixture::Submit((LPCSTR)root,(LPCSTR)damagedLegacy,(LPCSTR)image,"CopyTest:Folder:Nested:Game",0,0,'a');
  confirmReplacement(damagedJob);check(damagedJob->result().code!=0,"truncated MacBinary worker import rejected");
  volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen after rejected import");
  {char game[]="CopyTest:Folder:Nested:Game";auto f=hfs_open(volume,game);std::vector<BYTE> d(data.size()),r(resource.size());check(f&&hfs_read(f,d.data(),(unsigned long)d.size())==d.size()&&hfs_setfork(f,1)==0&&hfs_read(f,r.data(),(unsigned long)r.size())==r.size()&&hfs_close(f)==0&&d==data&&r==resource,"preflight rejection preserves existing destination through interface cleanup");}
  check(hfs_umount(volume)==0,"close preserved destination");
  auto legacyImport=TransferFixture::Submit((LPCSTR)root,(LPCSTR)legacyPath,(LPCSTR)image,"CopyTest:Legacy imported",0,0,'a');
  confirmReplacement(legacyImport);check(legacyImport->result().code==0,"automatic .bin worker import recognizes BinHex 5");
  auto exportedText=load(root+"\\Folder\\Text.txt");
  check(exportedText.size()==7 && !memcmp(exportedText.data(),"HELLO\r\n",7),"automatic text conversion unchanged");
  WIN32_FIND_DATAA entry={};entry.nFileSizeHigh=1;entry.nFileSizeLow=37;
  check(HostDirectoryEntrySize(entry)==0x100000025ULL,"Windows listing retains high file-size bits");
  entry.dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;entry.nFileSizeLow=12;
  check(HostDirectoryEntrySize(entry)==12,"directory entry count is not interpreted as a byte size");
  // Exercise the actual worker-owned recursive copy and folder-wide accounting.
  auto execute=[&](Storage::ConflictChoice choice) {
   auto job=TransferFixture::Submit((LPCSTR)image,"CopyTest:Folder",(LPCSTR)root,(LPCSTR)root,1,0,'a');
   unsigned conflicts=0;std::string pending;DWORD start=GetTickCount();
   while(!job->done()) {
    if(GetTickCount()-start>10000)throw std::runtime_error("Conflict test timed out");
    auto conflict=job->conflict();if(conflict.empty())pending.clear();else if(conflict!=pending){pending=conflict;++conflicts;job->resolve(choice);}
    Sleep(1);
   }
   return std::make_pair(job,conflicts);
  };
  CFile changed;check(changed.Open(root+"\\Folder\\Nested\\Game.bin",CFile::modeWrite|CFile::shareExclusive)!=FALSE,"open conflict fixture");
  changed.Write("CHANGED",7);changed.Close();
  auto readBefore=load(root+"\\Folder\\Nested\\Game.bin");
  auto textBefore=load(root+"\\Folder\\Text.txt");
  CFile unrelated;check(unrelated.Open(root+"\\Folder\\Unrelated",CFile::modeCreate|CFile::modeWrite)!=FALSE,"create unrelated file");unrelated.Write("KEEP",4);unrelated.Close();
  fprintf(out,"Testing skip\n");fflush(out);
  auto skipped=execute(Storage::ConflictChoice::Skip);
  check(skipped.first->result().code==0 && skipped.first->progress().skipped==2 && skipped.first->progress().files==0,"skip existing files and merge existing directories");
  check(skipped.first->progress().totalFiles==2 && skipped.first->progress().totalBytes==macbinary.size()+textBefore.size() &&
   skipped.first->progress().processed==skipped.first->progress().totalBytes,"folder total includes both forks and skipped work");
  check(load(root+"\\Folder\\Nested\\Game.bin")==readBefore,"skip leaves existing output untouched");
  fprintf(out,"Testing cancel\n");fflush(out);
  auto cancelled=execute(Storage::ConflictChoice::Cancel);
  check(cancelled.first->result().error==ERROR_CANCELLED && load(root+"\\Folder\\Nested\\Game.bin")==readBefore && load(root+"\\Folder\\Text.txt")==textBefore,"cancel preserves pre-existing output");
  fprintf(out,"Testing replace all\n");fflush(out);
  auto replaced=execute(Storage::ConflictChoice::ReplaceAll);
  check(replaced.first->result().code==0 && replaced.second==1 && replaced.first->progress().files==2 &&
   replaced.first->progress().processed==replaced.first->progress().totalBytes,"replace-all handles one conflict and completes the folder total");
  check(load(root+"\\Folder\\Nested\\Game.bin")==macbinary && load(root+"\\Folder\\Unrelated")==std::vector<BYTE>({'K','E','E','P'}),"overwrite replaces selected file and preserves unrelated files");
  // A failed replacement must preserve the previous file and the moved source.
  CString safePath=root+"\\preserved.bin";
  for(int fault : {1,2,3}) {
   {CFile f;check(f.Open(safePath,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create previous output");f.Write("ORIGINAL",8);f.Close();}
   auto work=TransferFixture::Work((LPCSTR)image,binary,(LPCSTR)root,(LPCSTR)safePath,0,1,'a');
   auto job=Storage::worker().submit([work,fault](Storage::Job& job){struct Reset{~Reset(){MacBrowser::windowsPublicationFault={};}} reset;
    if(fault)MacBrowser::windowsPublicationFault=[fault](const char* at){
     static const char* phases[]={"","before-data-write","before-data-flush","data-published"};
     if(!strcmp(at,phases[fault]))throw HfsPlus::Error("Injected Windows copy failure");
    };
    return work(job);});
   while(!job->done()){if(!job->conflict().empty())job->resolve(Storage::ConflictChoice::ReplaceAll);Sleep(1);}

   check(job->result().code!=0&&load(safePath)==std::vector<BYTE>({'O','R','I','G','I','N','A','L'}),"failed write/flush/publish preserves previous Windows output");
   volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen source after failed move");
   hfsdirent preserved;check(hfs_stat(volume,binary,&preserved)==0&&preserved.u.file.dsize==data.size()&&preserved.u.file.rsize==resource.size(),"failed move retains HFS source and both forks");
   check(hfs_umount(volume)==0,"close preserved source");
  }
  WIN32_FIND_DATAA leftover;HANDLE search=FindFirstFileA((root+"\\HFV*.tmp"),&leftover);
  check(search==INVALID_HANDLE_VALUE,"failed exports remove only their staged temporary files");
  fprintf(out,"PASS: failed Windows replacements preserve previous output and move source on write, flush and publication failures.\n");
  // Extension mapping must prompt for the final filename, never delete it after copying.
  CString mapping=root+"\\mapping.txt",mapped=root+"\\Game.safe",unmapped=root+"\\Game";
  {CFile f;check(f.Open(mapping,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create mapping");const char line[]="safe,,APPL,TEST,x,,test\r\n";f.Write(line,sizeof(line)-1);f.Close();}
  import_map(mapping);
  for(auto choice : {Storage::ConflictChoice::Skip,Storage::ConflictChoice::Cancel,Storage::ConflictChoice::ReplaceAll}) {
   for(const auto& path : {mapped,unmapped}){CFile f;check(f.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create mapped conflict");f.Write("KEEP",4);f.Close();}
   auto job=TransferFixture::Submit((LPCSTR)image,binary,(LPCSTR)root,(LPCSTR)root,0,0,'r');
   unsigned conflicts=0;std::string pending;
   while(!job->done()) {
    auto conflict=job->conflict();if(!conflict.empty()&&conflict!=pending){pending=conflict;++conflicts;check(conflict=="Game.safe","conflict names mapped destination");job->resolve(choice);}Sleep(1);
   }
   if(conflicts!=1){fprintf(out,"Mapped export diagnostic: conflicts=%u code=%d error=%lu message=%s\n",conflicts,job->result().code,job->result().error,job->result().message.c_str());fflush(out);}
   check(conflicts==1,"mapped export asks before replacing final output");
   check(load(unmapped)==std::vector<BYTE>({'K','E','E','P'}),"raw mapping does not alter intermediate filename");
   if(choice==Storage::ConflictChoice::ReplaceAll)check(job->result().code==0&&load(mapped)==data,"mapped replacement publishes exact data fork");
   else check(load(mapped)==std::vector<BYTE>({'K','E','E','P'}),"mapped skip/cancel preserves previous file");
  }
  tmap_final();
  fprintf(out,"PASS: raw extension mapping checks the final output conflict; skip/cancel preserves it and replacement preserves unrelated files.\n");
  CString hostDirectory=root+"\\HostCopy",hostSource=root+"\\HostFile",hostDestination=hostDirectory+"\\HostFile";
  check(CreateDirectoryA(hostDirectory,nullptr)!=FALSE,"create Windows copy target");
  {CFile f;check(f.Open(hostSource,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows source");f.Write("NEW",3);f.Close();}
  {CFile f;check(f.Open(hostSource+":Finder",CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows alternate stream");f.Write("FORK",4);f.Close();}
  for(int fault : {1,2,3,0}) {
   {CFile f;check(f.Open(hostDestination,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create Windows destination");f.Write("OLD",3);f.Close();}
   {CFile f;check(f.Open(hostDestination+":Finder",CFile::modeCreate|CFile::modeWrite)!=FALSE,
        "create destination alternate stream");f.Write("KEEP",4);f.Close();}
   auto work=TransferFixture::Work((LPCSTR)root,(LPCSTR)hostSource,(LPCSTR)root,(LPCSTR)hostDirectory,0,0,'a');
   auto job=Storage::worker().submit([work,fault](Storage::Job& job){struct Reset{~Reset(){MacBrowser::windowsPublicationFault={};}} reset;
    if(fault)MacBrowser::windowsPublicationFault=[fault](const char* at){
     static const char* phases[]={"","before-data-write","before-data-flush","data-published"};
     if(!strcmp(at,phases[fault]))throw HfsPlus::Error("Injected Windows copy failure");
    };
    return work(job);});
   while(!job->done()){if(!job->conflict().empty())job->resolve(Storage::ConflictChoice::ReplaceAll);Sleep(1);}
   check((job->result().code!=0)==(fault!=0),"Windows staged copy result");
   check(load(hostDestination)==(fault?std::vector<BYTE>({'O','L','D'}):std::vector<BYTE>({'N','E','W'})),"Windows copy failure preserves previous destination");
   check(load(hostDestination+":Finder")==(fault?std::vector<BYTE>({'K','E','E','P'}):std::vector<BYTE>({'F','O','R','K'})),
         "Windows publication preserves the original named stream on failure and copies the source stream on success");
  }
  check(load(hostDestination+":Finder")==std::vector<BYTE>({'F','O','R','K'}),"Windows staging preserves alternate streams");
  auto sameHost=TransferFixture::Submit((LPCSTR)root,(LPCSTR)hostSource,(LPCSTR)root,(LPCSTR)root,0,1,'a');
  sameHost->wait();check(sameHost->result().code!=0&&load(hostSource)==std::vector<BYTE>({'N','E','W'}),"Windows self-move preserves original file");
  fprintf(out,"PASS: Windows copies stage replacements and preserve existing output on write/flush/publication failures; self-move retains source.\n");
  auto failed=TransferFixture::Submit((LPCSTR)image,"CopyTest:Missing",(LPCSTR)root,(LPCSTR)root,0,0,'a');
  failed->wait();check(failed->result().code!=0,"source scan errors reach completion");
  auto hostFailure=TransferFixture::Submit((LPCSTR)root,(LPCSTR)(root+"\\DoesNotExist"),(LPCSTR)root,(LPCSTR)(root+"\\NoOutput"),0,0,'a');
  hostFailure->wait();check(hostFailure->result().code!=0&&!hostFailure->result().message.empty()&&hostFailure->result().message!="no error","missing host copy reports meaningful failure without stale library errors");
  // Production copy job -> working HFS image -> locked physical write/readback.
  std::vector<BYTE> blank;std::string floppyError;
  check(PrepareFloppyHfs("FloppyCopy",blank,floppyError),"prepare ordinary-copy floppy");
  readonly_cd_backend_init();char floppyPath[64];get_floppy_volume_file_name(25,floppyPath);
  {
   CopyFloppy buffered(blank);DirectFloppy direct(&buffered);check(direct.Open(25),"open direct buffer fixture");
   BYTE changed[1024],readBack[1024];memset(changed,0x5a,sizeof(changed));unsigned reads=buffered.disk.reads;
   check(direct.Write(512,512,changed)&&direct.Write(1024,512,changed+512)&&buffered.writes==0,"adjacent writes accumulate in original-style writeback buffer");
   check(direct.Read(512,sizeof(readBack),readBack)&&!memcmp(changed,readBack,sizeof(changed))&&buffered.writes==0&&buffered.disk.reads==reads,"read-your-writes uses coherent sector cache before flush");
   bool protectedOtherThread=false;std::thread other([&]{protectedOtherThread=readonly_media_path(floppyPath)&&fhook_open(floppyPath,O_RDWR)<0&&errno==EROFS;});other.join();
   check(protectedOtherThread,"write permission is scoped to the copy worker thread");
   check(direct.Finish()&&buffered.writes==1&&buffered.disk.reads==reads&&!memcmp(buffered.disk.media.data()+512,changed,sizeof(changed)),"one contiguous buffered write, no readback pass, unsupported flush accepted with write-through I/O");
  }
  check(readonly_media_path(floppyPath)&&!DirectFloppy::Current(25),"read-only browsing restored after direct scope");
  {CopyFloppy verified(blank);DirectFloppy direct(&verified);BYTE sector[512];memset(sector,0x6b,sizeof(sector));check(direct.Open(25,true)&&direct.Write(512,sizeof(sector),sector),"prepare verified direct write");auto bytes=verified.disk.requestedReadBytes;check(direct.Finish()&&verified.disk.requestedReadBytes==bytes+sizeof(sector),"verified move must read only changed sectors from the device");}
  {CopyFloppy failedReadback(blank);DirectFloppy direct(&failedReadback);BYTE sector[512]{};check(direct.Open(25,true)&&direct.Write(512,sizeof(sector),sector),"prepare failing verified write");failedReadback.failure=4;check(!direct.Finish()&&direct.Error()==ERROR_CRC,"cached read-your-writes masked a failed device readback");}
  {
   CopyFloppy swapped(blank);DirectFloppy direct(&swapped);BYTE sector[512]={};check(direct.Open(25)&&direct.Write(0,sizeof(sector),sector),"buffer before media swap");++swapped.disk.changeCount;
   check(!direct.Finish()&&direct.Error()==ERROR_MEDIA_CHANGED&&swapped.writes==0,"media change prevents buffered writes from reaching replacement disk");
  }
  auto physicalCopy=[&](CopyFloppy& api,const std::string& fromVolume,const std::string& from,int dir,int move=0) {
   check(readonly_floppy_attach_api(25,&api),"attach copy floppy");
   auto job=TransferFixture::Submit(fromVolume,from,floppyPath,"FloppyCopy:",dir,move,'a');
   job->wait();return job->result();
  };
  CopyFloppy physical(blank);
  auto physicalResult=physicalCopy(physical,(LPCSTR)image,"CopyTest:Folder",1);
  unsigned changedTracks=0;for(size_t offset=0;offset<blank.size();offset+=9216)if(memcmp(blank.data()+offset,physical.disk.media.data()+offset,9216))++changedTracks;
  check(physicalResult.code==0&&!physicalResult.moved&&changedTracks>0&&changedTracks<160&&physical.writes>0&&physical.disk.requestedReadBytes<blank.size()&&!physical.locked,"recursive direct copy avoids whole-disk passes, flushes buffered writes and retains source");
  check(readonly_floppy_attach_api(25,&physical),"reattach populated floppy");
  CDSKFile browsing;check(browsing.Open(floppyPath,CFile::modeRead)!=FALSE,"keep browsing handle during floppy copy");
  volume=hfs_mount(floppyPath,0,O_RDONLY);check(volume!=nullptr,"mount physical copy output");
  char copied[]="FloppyCopy:Folder:Nested:Game";file=hfs_open(volume,copied);check(file!=nullptr,"nested file on floppy");
  std::vector<BYTE> readData(data.size()),readResource(resource.size());
  check(hfs_read(file,readData.data(),(unsigned long)readData.size())==readData.size()&&hfs_setfork(file,1)==0&&
   hfs_read(file,readResource.data(),(unsigned long)readResource.size())==readResource.size()&&hfs_close(file)==0&&hfs_umount(volume)==0&&readData==data&&readResource==resource,"floppy preserves both forks");
  // Import a Windows MacBinary file while a GUI reader remains open.
  CString imported=root+"\\Folder\\Nested\\Game.bin";
  auto importJob=TransferFixture::Submit((LPCSTR)root,(LPCSTR)imported,floppyPath,"FloppyCopy:",0,0,'a');importJob->wait();
  check(importJob->result().code==0&&!importJob->result().moved&&load(imported)==macbinary,"Windows import retains source until verified and preserves original source");
  BYTE signature[2];check(browsing.Seek(1024,CFile::begin)==1024&&browsing.Read(signature,2)==2&&signature[0]==0x42&&signature[1]==0x44,"existing GUI reader resumes after locked write");browsing.Close();
  volume=hfs_mount(floppyPath,0,O_RDONLY);check(volume!=nullptr,"mount imported floppy");
  char importedName[]="FloppyCopy:Game";file=hfs_open(volume,importedName);check(file!=nullptr,"imported MacBinary filename");
  hfsdirent retained;check(hfs_stat(volume,copied,&retained)==0,"later import retains previously copied folder and file");
  check(hfs_read(file,readData.data(),(unsigned long)readData.size())==readData.size()&&hfs_setfork(file,1)==0&&hfs_read(file,readResource.data(),(unsigned long)readResource.size())==readResource.size()&&hfs_close(file)==0&&hfs_umount(volume)==0&&readData==data&&readResource==resource,"Windows MacBinary import preserves forks and existing folder");
  for(int failure=1;failure<=6;++failure){
   CopyFloppy bad(blank);bad.failure=failure;auto result=physicalCopy(bad,(LPCSTR)image,"CopyTest:Folder",1,1);
   check(result.code!=0&&result.message!="no error"&&!result.message.empty()&&!result.moved,"physical copy errors are meaningful and retain source");
   if(failure<=2)check(bad.writes==0&&bad.disk.media==blank,"write-protect/lock failure leaves disk untouched");
   if(failure==5)check(bad.writes==0&&result.error==ERROR_MEDIA_CHANGED,"changed disk rejected before writing");
   if(failure==3)check(result.error==ERROR_WRITE_FAULT,"short floppy write reported");
   if(failure==4)check(result.error==ERROR_CRC,"read error reported");
   if(failure==6)check(result.error==ERROR_WRITE_FAULT,"real flush failure is reported");
   check(!bad.locked&&bad.disk.opens==bad.disk.closes,"physical failure closes and unlocks handles");
   check(readonly_floppy_attach_api(25,nullptr),"detach failed copy floppy");
  }
  CString tooBig=root+"\\TooBig";CFile oversized;check(oversized.Open(tooBig,CFile::modeCreate|CFile::modeReadWrite)!=FALSE,"create insufficient-space source");oversized.SetLength(2*1024*1024);oversized.Close();
  CopyFloppy full(blank);auto noSpace=physicalCopy(full,(LPCSTR)root,(LPCSTR)tooBig,0);
  check(noSpace.code!=0&&noSpace.message!="no error"&&!full.locked&&full.disk.opens==full.disk.closes,"disk full reports failure and releases the direct writer");
  check(readonly_floppy_attach_api(25,nullptr),"detach full fixture");
  fprintf(out,"PASS: ordinary physical floppy copies: nested folders, HFS and Windows sources, both forks, existing-reader reopening, direct buffered writes without full-disk passes, write protection, locking, short writes, read errors, swapped media, unsupported flush and source retention; simulated devices only.\n");fflush(out);
  CString hugeDirectory=root+"\\HugeTarget";check(CreateDirectoryA(hugeDirectory,nullptr)!=FALSE,"create large-output conflict folder");
  CString hugePath=hugeDirectory+"\\Game.bin";HANDLE huge=CreateFileA(hugePath,GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
  check(huge!=INVALID_HANDLE_VALUE,"create sparse large conflict");DWORD ignored;LARGE_INTEGER length;length.QuadPart=0x80001000ULL;
  check(DeviceIoControl(huge,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&ignored,nullptr)&&SetFilePointerEx(huge,length,nullptr,FILE_BEGIN)&&SetEndOfFile(huge),"size sparse conflict above 2 GiB");CloseHandle(huge);
  auto largeConflict=TransferFixture::Submit((LPCSTR)image,"CopyTest:Folder:Nested:Game",(LPCSTR)hugeDirectory,(LPCSTR)hugeDirectory,0,0,'a');
  DWORD deadline=GetTickCount();bool asked=false;
  while(!largeConflict->done()){
   if(!largeConflict->conflict().empty()){asked=true;largeConflict->resolve(Storage::ConflictChoice::Skip);}
   if(GetTickCount()-deadline>10000)throw std::runtime_error("Large conflict timed out");
   Sleep(1);
  }
  huge=CreateFileA(hugePath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  LARGE_INTEGER actual;check(asked&&largeConflict->result().code==0&&GetFileSizeEx(huge,&actual)&&actual.QuadPart==length.QuadPart,"large existing output requires overwrite decision and remains unchanged on Skip");CloseHandle(huge);
  CWnd owner;CString ownerClass=AfxRegisterWndClass(0);
  check(owner.CreateEx(0,ownerClass,"Hidden copy owner",WS_OVERLAPPED,0,0,100,100,nullptr,0)!=FALSE,"create hidden copy owner");
  AfxGetApp()->m_pMainWnd=&owner;InitializeStorageUi();storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;
  {
   CDynCopyModeSelect dialog;dialog.m_outcopy=1;dialog.m_copy_mode=7;
   check(dialog.Create(IDD_DYNAMIC_COPYMODE_SELECT,&owner)!=FALSE,"create copy mode dialog");
   check(dialog.SendDlgItemMessage(IDC_COPY_MODE,CB_GETCOUNT)==8 && dialog.SendDlgItemMessage(IDC_COPY_MODE,CB_GETCURSEL)==6,"BinHex 5 dynamic selection mapping");
   check(dialog.UpdateData(TRUE)&&dialog.m_copy_mode==7,"BinHex 5 saved mode mapping");dialog.DestroyWindow();
   COptionsPage2 options;options.m_copyout_mode=7;options.m_copyin_mode=7;
   check(options.Create(IDD_OPTIONS_PAGE_2,&owner)!=FALSE,"create transfer options");
   check(options.SendDlgItemMessage(IDC_COPYOUT_MODE,CB_GETCOUNT)==9&&options.SendDlgItemMessage(IDC_COPYIN_MODE,CB_GETCOUNT)==9&&options.SendDlgItemMessage(IDC_COPYOUT_MODE,CB_GETCURSEL)==7&&options.SendDlgItemMessage(IDC_COPYIN_MODE,CB_GETCURSEL)==7,"persistent BinHex 5 choices preserve ask index");
   options.DestroyWindow();HFSIFACE_set_modeout(7);check(HFSIFACE_copy_mode()=='5',"BinHex 5 mode dispatch");HFSIFACE_set_modeout(0);
  }
  for(const bool single:{false,true}){
   storageUiTest={};
   const auto percentage=ShowStorageJob([](Storage::Job& job){
    Storage::Progress progress;progress.phase="Copying";progress.scanning=false;
    progress.totalBytes=100;progress.processed=42;progress.fileTotal=100;progress.fileBytes=66;
    job.progress(progress);
    return Storage::Result{};
   },"Hidden percentage layout",true,single);
   check(percentage.code==0&&storageUiTest.overallPercent==42&&
         storageUiTest.filePercent==(single?-1:66)&&storageUiTest.copyBarCount==(single?1u:2u),
         "progress percentages match single-file and folder bars");
  }
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;
  auto uiCopy=ShowStorageJob(TransferFixture::Work((LPCSTR)image,"CopyTest:Folder",(LPCSTR)root,(LPCSTR)root,1,0,'a'),"Hidden copy integration",true);
  check(uiCopy.code==0 && storageUiTest.disabledDuring && storageUiTest.enabledAfter && storageUiTest.dialogClosed && owner.IsWindowEnabled()&&storageUiTest.copyBarCount==2,"folder copy retains both bars and restores main window");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;
  auto uiSingle=ShowStorageJob(TransferFixture::Work((LPCSTR)image,"CopyTest:Folder:Nested:Game",(LPCSTR)root,(LPCSTR)root,0,0,'a'),"Hidden single file",true,true);
  check(uiSingle.code==0&&storageUiTest.copyBarCount==1&&storageUiTest.dialogCreations==1&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"single file has one bar and restores owner");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Skip;
  auto uiSmallFolder=ShowStorageJob(TransferFixture::Work((LPCSTR)image,"CopyTest:Folder:Nested",(LPCSTR)root,(LPCSTR)root,1,0,'a'),"Hidden one-file folder",true);
  check(uiSmallFolder.code==0&&storageUiTest.copyBarCount==2,"folder containing one file still has both bars");
  check(readonly_floppy_attach_api(25,&physical),"attach populated floppy for full dialog lifecycle");
  storageUiTest={};
  auto uiFloppy=ShowStorageJob(TransferFixture::Work((LPCSTR)image,"CopyTest:Folder:Text",floppyPath,"FloppyCopy:",0,0,'a'),"Hidden floppy copy cycles",true,true);
  check(uiFloppy.code==0&&storageUiTest.dialogCreations==1&&storageUiTest.copyBarCount==1&&storageUiTest.ownerRestoredBeforeClose&&owner.IsWindowEnabled(),"all floppy read/check/write/verify phases share one single-file dialog and restore owner");
  check(readonly_floppy_attach_api(25,nullptr),"detach dialog lifecycle floppy");
  storageUiTest={};storageUiTest.answer=Storage::ConflictChoice::Cancel;
  auto uiCancel=ShowStorageJob(TransferFixture::Work((LPCSTR)image,"CopyTest:Folder",(LPCSTR)root,(LPCSTR)root,1,0,'a'),"Hidden cancelled copy",true);
  check(uiCancel.error==ERROR_CANCELLED && storageUiTest.enabledAfter && owner.IsWindowEnabled(),"cancelled copy/conflict completion restores main window");
  owner.DestroyWindow();AfxGetApp()->m_pMainWnd=nullptr;
  fprintf(out,"PASS: BinHex 5 worker import/export and preflight preservation, copy/options mode mapping; recursive Automatic export, nested folders, both forks, text conversion, exact output byte/file progress and failure accounting, worker execution, directory totals, overwrite/skip/cancel/replace-all and source errors, hidden copy/conflict completion restoration, one bar for single files and two for folders (including one-file folders), one dialog across physical-copy phases; no physical devices.\n");
 } catch(const std::exception& error) {fprintf(out,"FAIL: %s\n",error.what());fclose(out);return 1;} catch(const char* error) {fprintf(out,"FAIL: %s (HFS: %s)\n",error,hfs_error?hfs_error:"");fclose(out);return 1;}
 fclose(out);return 0;
}
#endif
