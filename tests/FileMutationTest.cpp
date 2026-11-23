// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "VolumeCoordinator.h"
#include "StorageWorker.h"
#include <cstdio>
#include <fcntl.h>
#include <vector>
#include <stdexcept>
#include <winioctl.h>
#include "RenameName.h"
#include "WindowsEntryIdentity.h"
#include "MacBrowserModel.h"
extern int fat_rename(char*,char*);
extern std::function<void()> windowsDeleteEnumeratedForTest;
extern "C" {
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/Replacement.h"
#include "hfs/interface.h"
int do_delete_directory(hfsvol*,char*);
}
void RunClassicWriteFailureTests(const char*,FILE*);
void RunDurableWindowsMutationTests(const char*,FILE*);
namespace {
bool junction(const CString& link,const CString& target) {
 if(!CreateDirectoryA(link,nullptr))return false;
 HANDLE handle=CreateFileA(link,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(handle==INVALID_HANDLE_VALUE)return false;
 CStringW wide(target);std::wstring print(wide.GetString());std::wstring substitute=L"\\??\\"+print;
 struct Buffer {DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;wchar_t path[1024];} data={};
 data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subLength=WORD(substitute.size()*2);data.printOffset=WORD(data.subLength+2);data.printLength=WORD(print.size()*2);
 memcpy(data.path,substitute.c_str(),data.subLength+2);memcpy(reinterpret_cast<BYTE*>(data.path)+data.printOffset,print.c_str(),data.printLength+2);
 data.length=WORD(8+data.printOffset+data.printLength+2);DWORD returned;
 bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&returned,nullptr)!=FALSE;CloseHandle(handle);return ok;
}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<BYTE> load(const CString& path){CFile f;check(f.Open(path,CFile::modeRead)!=FALSE,"open fixture");std::vector<BYTE> b(size_t(f.GetLength()));check(f.Read(b.data(),UINT(b.size()))==b.size(),"read fixture");return b;}
void save(const CString& path,const char* bytes){CFile f;check(f.Open(path,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create fixture");f.Write(bytes,UINT(strlen(bytes)));}
void checkClassicMountResolution(const CString& root,FILE* report) {
 char label[]="Twin",otherLabel[]="Other",type[]="TEXT",creator[]="TEST";
 const auto mount=[&](CString image,char* volumeLabel){
  {
   CFile f;
   check(f.Open(image,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create mount identity fixture");
   f.SetLength(2*1024*1024);
  }
  auto* path=image.GetBuffer();
  check(hfs_format(path,0,volumeLabel)==0,"format mount identity fixture");
  auto* volume=hfs_mount(path,0,O_RDWR);
  image.ReleaseBuffer();
  check(volume!=nullptr,"mount identity fixture");
  return volume;
 };
 auto* a=mount(root+"\\twin-a.dsk",label);
 auto* b=mount(root+"\\twin-b.dsk",label);
 auto* other=mount(root+"\\other-label.dsk",otherLabel);
 const auto put=[&](hfsvol* volume,char* name,char byte){
  check(hfs_create(volume,name,type,creator)==0,"create entry on selected mount");
  auto* file=hfs_open(volume,name);
  check(file&&hfs_write(file,&byte,1)==1&&hfs_close(file)==0,"write selected mount data");
 };
 const auto read=[](hfsvol* volume,char* name){
  auto* file=hfs_open(volume,name);
  char byte=0;
  check(file&&hfs_read(file,&byte,1)==1&&hfs_close(file)==0,"read selected mount data");
  return byte;
 };
 char relative[]="File",absolute[]="twin:File",foreign[]="Other:File";
 put(a,relative,'A');put(b,relative,'B');put(other,relative,'C');
 hfs_setvol(b);
 check(read(a,absolute)=='A'&&read(b,absolute)=='B'&&read(nullptr,absolute)=='B',
       "the current or newer same-label mount displaced an explicit selection");
 hfsdirent entry{};
 check(hfs_stat(a,foreign,&entry)<0&&errno==ENOENT,"foreign label resolved on another mount");
 char foreignOutput[]="Other:New";
 check(hfs_create(a,foreignOutput,type,creator)<0&&errno==ENOENT&&
       hfs_stat(other,foreignOutput,&entry)<0,"wrong-label creation reached another mount");
 check(hfs_rename(a,absolute,foreignOutput)<0&&read(a,absolute)=='A'&&read(other,foreign)=='C',
       "wrong-label rename changed one of the mounts");

 char firstTemporary[]="HFV00000001.tmp",thirdBackup[]="HFV00000003.bak";
 put(a,firstTemporary,'T');put(a,thirdBackup,'K');
 HfsReplacement left{},right{};
 auto* leftFile=replacement_open(&left,a,absolute,type,creator);
 auto* rightFile=replacement_open(&right,b,absolute,type,creator);
 check(leftFile&&rightFile&&left.active&&right.active,"stage replacements on independent mounts");
 check(!strcmp(left.temporary,"HFV00000002.tmp")&&!strcmp(left.backup,"HFV00000004.bak")&&
       !strcmp(right.temporary,"HFV00000001.tmp")&&!strcmp(right.backup,"HFV00000002.bak"),
       "replacement names did not start independently or avoid existing names");
 char leftByte='X',rightByte='Y';
 check(hfs_write(leftFile,&leftByte,1)==1&&hfs_write(rightFile,&rightByte,1)==1&&
       hfs_close(leftFile)==0&&hfs_close(rightFile)==0,"write staged replacements");
 int leftResult=0,rightResult=0;
 char* error=nullptr;
 replacement_finish(&right,&rightResult,&error);
 replacement_finish(&left,&leftResult,&error);
 check(!leftResult&&!rightResult&&read(a,absolute)=='X'&&read(b,absolute)=='Y'&&
       read(a,firstTemporary)=='T'&&read(a,thirdBackup)=='K'&&read(other,foreign)=='C',
       "replacement touched another mount or an existing temporary or backup name");
 check(hfs_stat(a,left.temporary,&entry)<0&&hfs_stat(a,left.backup,&entry)<0&&
       hfs_stat(b,right.temporary,&entry)<0&&hfs_stat(b,right.backup,&entry)<0,
       "replacement left its own temporary or backup file behind");
 check(hfs_umount(a)==0&&hfs_umount(b)==0&&hfs_umount(other)==0,"close mount identity fixtures");
 fprintf(report,"PASS: classic HFS absolute paths stay on the selected mount, and independent staged replacements preserve name collisions.\n");
}

}
int RunFileMutationTest(const char* directory) {
 if(!CreateDirectoryA(directory,nullptr))return 2;
 CString root=directory,image=root+"\\mutations.dsk";FILE* report=nullptr;fopen_s(&report,(root+"\\file-mutations-report.txt"),"w");if(!report)return 2;
 int silent=HFSIFACE_silencer(1);
 try {
  RunDurableWindowsMutationTests(directory,report);
  RunClassicWriteFailureTests(directory,report);
  checkClassicMountResolution(root,report);
  {CFile f;check(f.Open(image,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create HFS fixture");f.SetLength(4*1024*1024);}
  char label[]="MutationTest",fileName[]="MutationTest:Keep",type[]="TEXT",creator[]="TEST";
  check(hfs_format(image.GetBuffer(),0,label)==0,"format mutation fixture");image.ReleaseBuffer();
  hfsvol* volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount fixture");
  check(hfs_create(volume,fileName,type,creator)==0,"create preserved file");hfsfile* file=hfs_open(volume,fileName);
  check(file&&hfs_write(file,const_cast<char*>("DATA"),4)==4&&hfs_setfork(file,1)==0&&hfs_write(file,const_cast<char*>("RESOURCE"),8)==8&&hfs_close(file)==0&&hfs_umount(volume)==0,"write both preserved forks");
  volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount folder fixture");
  char folder[]="MutationTest:Remove",child[]="MutationTest:Remove:Keep";
  check(hfs_mkdir(volume,folder)==0&&hfs_create(volume,child,type,creator)==0&&hfs_umount(volume)==0,"create folder with same-named child");
  volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount folder deletion");
  char trailing[]="MutationTest:Remove:";
  check(do_delete_directory(volume,trailing)==0,"delete folder with a trailing colon");
  hfsdirent retained;check(hfs_stat(volume,fileName,&retained)==0&&retained.u.file.dsize==4&&retained.u.file.rsize==8,"folder deletion escaped to same-named file in parent");
  check(hfs_umount(volume)==0,"close folder deletion");
  auto before=load(image);char volumeRoot[]="MutationTest:";
  volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount root-delete fixture");
  hfsdirent keep;check(hfs_stat(volume,fileName,&keep)==0,"source exists before root delete");
  check(do_delete_directory(volume,volumeRoot)!=0&&hfs_error&&strstr(hfs_error,"volume root"),"root delete must fail before traversal");
  check(hfs_umount(volume)==0,"close root-delete fixture");
  check(load(image)==before,"rejected HFS root deletion changed volume contents");
  volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount open-file mutation fixture");
  file=hfs_open(volume,fileName);check(file!=nullptr,"hold an HFS file open");
  char renamed[]="MutationTest:Renamed";
  check(hfs_delete(volume,fileName)<0,"deleting an open HFS file was accepted");
  check(hfs_rename(volume,fileName,renamed)<0,"renaming an open HFS file was accepted");
  check(hfs_close(file)==0,"close preserved HFS file");
  check(hfs_rename(volume,fileName,renamed)==0&&hfs_rename(volume,renamed,fileName)==0,"closed HFS file cannot be renamed");
  file=hfs_open(volume,fileName);char data[8];check(file&&hfs_read(file,data,4)==4&&!memcmp(data,"DATA",4)&&hfs_setfork(file,1)==0&&hfs_read(file,data,8)==8&&!memcmp(data,"RESOURCE",8),"open-file mutation changed fork data");
  check(hfs_close(file)==0&&hfs_umount(volume)==0,"finish open-file mutation test");
  fprintf(report,"PASS: HFS delete/rename rejects open files and preserves both forks; rename succeeds after close.\n");
  // A long replacement key can split a leaf after the source key was deleted.
  CString fullImage=root+"\\full-rename.dsk";
  {CFile f;check(f.Open(fullImage,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create full catalog fixture");f.SetLength(4*1024*1024);}
  check(hfs_format(fullImage.GetBuffer(),0,label)==0,"format full catalog fixture");fullImage.ReleaseBuffer();
  volume=hfs_mount(fullImage.GetBuffer(),0,O_RDWR);fullImage.ReleaseBuffer();check(volume!=nullptr,"mount full catalog fixture");
  for(int i=0;i<64;++i){char name[64];sprintf_s(name,"MutationTest:File%08d",i);check(hfs_create(volume,name,type,creator)==0,"fill catalog leaves");}
  check(hfs_umount(volume)==0,"close populated catalog");auto snapshot=load(fullImage);unsigned rejected=0;
  for(int i=0;i<64;++i) {
   {CFile f;check(f.Open(fullImage,CFile::modeCreate|CFile::modeWrite)!=FALSE,"reset full catalog fixture");f.Write(snapshot.data(),UINT(snapshot.size()));}
   volume=hfs_mount(fullImage.GetBuffer(),0,O_RDWR);fullImage.ReleaseBuffer();check(volume!=nullptr,"mount catalog failure case");
   auto freeNodes=volume->cat.hdr.bthFree;auto freeBlocks=volume->mdb.drFreeBks;
   volume->cat.hdr.bthFree=0;volume->mdb.drFreeBks=0; // Inject exhausted catalog reserve/allocator.
   char source[64],destination[80];sprintf_s(source,"MutationTest:File%08d",i);sprintf_s(destination,"MutationTest:File%08dabcdefghijklmnopqrs",i);
   int result=hfs_rename(volume,source,destination);hfsdirent entry;bool retained=hfs_stat(volume,source,&entry)==0;
   volume->cat.hdr.bthFree=freeNodes;volume->mdb.drFreeBks=freeBlocks;
   check(hfs_umount(volume)==0,"close catalog failure case");
   if(result<0){++rejected;check(retained,"failed rename removed its original catalog entry");}
  }
  check(rejected>0,"catalog exhaustion regression did not exercise a failed rename");
  fprintf(report,"PASS: failed catalog-space renames preserve the source entry.\n");
  CString missingImage=root+"\\missing.dsk";
  check(HFSIFACE_delete(missingImage.GetBuffer(),fileName)!=0,"missing-volume delete reported success");missingImage.ReleaseBuffer();
  check(HFSIFACE_rmdir(missingImage.GetBuffer(),folder)!=0,"missing-volume folder delete reported success");missingImage.ReleaseBuffer();
  check(HFSIFACE_rename(missingImage.GetBuffer(),fileName,renamed)!=0,"missing-volume rename reported success");missingImage.ReleaseBuffer();
  std::string oversizedPath(400,'x');
  check(HFSIFACE_delete(image.GetBuffer(),const_cast<char*>(oversizedPath.c_str()))!=0,"oversized delete path accepted");image.ReleaseBuffer();
  check(HFSIFACE_rename(image.GetBuffer(),fileName,const_cast<char*>(oversizedPath.c_str()))!=0,"oversized rename path accepted");image.ReleaseBuffer();
  fprintf(report,"PASS: mutation wrappers reject missing volumes and oversized paths instead of reporting success.\n");
  volume=hfs_mount(image.GetBuffer(),0,O_RDWR);image.ReleaseBuffer();check(volume!=nullptr,"mount rename-collision fixture");
  char destinationFolder[]="MutationTest:Destination",folderBefore[]="MutationTest:FolderBefore",folderAfter[]="MutationTest:FolderAfter",folderChild[]="MutationTest:FolderBefore:Child";
  check(hfs_mkdir(volume,destinationFolder)==0&&hfs_mkdir(volume,folderBefore)==0&&hfs_create(volume,folderChild,type,creator)==0&&hfs_umount(volume)==0,"create folder rename fixtures");
  check(HFSIFACE_rename(image.GetBuffer(),fileName,destinationFolder)!=0,"label rename moved a file inside an existing folder");image.ReleaseBuffer();
  char upperName[]="MutationTest:KEEP";
  check(HFSIFACE_rename(image.GetBuffer(),fileName,upperName)==0,"case-only HFS rename rejected");image.ReleaseBuffer();
  check(HFSIFACE_rename(image.GetBuffer(),upperName,fileName)==0,"case-only HFS rename restoration rejected");image.ReleaseBuffer();
  check(HFSIFACE_rename(image.GetBuffer(),folderBefore,folderAfter)==0,"populated HFS folder rename failed");image.ReleaseBuffer();
  volume=hfs_mount(image.GetBuffer(),0,O_RDONLY);image.ReleaseBuffer();check(volume!=nullptr,"reopen renamed folder");
  char movedChild[]="MutationTest:FolderAfter:Child";hfsdirent renamedEntry;check(hfs_stat(volume,movedChild,&renamedEntry)==0&&hfs_stat(volume,fileName,&renamedEntry)==0,"folder rename detached children or collision moved source");check(hfs_umount(volume)==0,"close renamed folder");
  fprintf(report,"PASS: label renames reject existing folders as destinations; case-only and populated-folder renames preserve entries.\n");
  CString external=root+"\\Outside",selected=root+"\\Selected",link=selected+"\\Junction";
  check(CreateDirectoryA(external,nullptr)&&CreateDirectoryA(selected,nullptr),"create directory deletion fixtures");
  save(external+"\\Keep","OUTSIDE");save(selected+"\\.hidden","DELETE");
  check(junction(link,external),"create unprivileged directory junction fixture");
  const auto hostPath=[](const CString& path){return VolumeCoord::Wide((LPCSTR)path);};
  const auto removeHost=[&](const CString& path,bool directory){
   const auto wide=hostPath(path);
   return DeleteHostSelectionJob(wide,directory,MacBrowser::DescribeWindowsFile(wide).identity,nullptr);
  };
  check(removeHost(selected,true)!=0,"delete selected tree containing a junction");
  check(GetFileAttributesA(selected)==INVALID_FILE_ATTRIBUTES&&load(external+"\\Keep")==std::vector<BYTE>({'O','U','T','S','I','D','E'}),"recursive Windows deletion followed a junction outside its target");
  const CString swapFolder=root+"\\SwapChild",swapChild=swapFolder+"\\Document",savedChild=root+"\\OriginalChild";
  check(CreateDirectoryA(swapFolder,nullptr)!=FALSE,"create replacement child fixture");
  save(swapChild,"ORIGINAL");
  {
   bool replaced=false;
   windowsDeleteEnumeratedForTest=[&]{
    check(MoveFileA(swapChild,savedChild)!=FALSE,"move enumerated child out of the selected folder");
    save(swapChild,"REPLACED");
    replaced=true;
   };
   struct ClearFault{~ClearFault(){windowsDeleteEnumeratedForTest={};}} clear;
   const auto result=removeHost(swapFolder,true);
   check(replaced&&!result&&GetLastError()==ERROR_FILE_INVALID,
         "recursive delete did not refuse a replaced child");
  }
  check(load(swapChild)==std::vector<BYTE>({'R','E','P','L','A','C','E','D'})&&
        load(savedChild)==std::vector<BYTE>({'O','R','I','G','I','N','A','L'}),
        "recursive delete removed either version of a changed child");
  fprintf(report,"PASS: legacy recursive delete verifies enumerated children by handle and preserves replacements.\n");
  CString protectedDirectory=root+"\\Locked";check(CreateDirectoryA(protectedDirectory,nullptr)!=FALSE,"create locked deletion fixture");
  save(protectedDirectory+"\\Keep","LOCKED");check(SetFileAttributesA(protectedDirectory+"\\Keep",FILE_ATTRIBUTE_READONLY)!=FALSE,"protect deletion fixture");
  check(!removeHost(protectedDirectory,true)&&GetFileAttributesA(protectedDirectory+"\\Keep")!=INVALID_FILE_ATTRIBUTES,"protected child deletion must fail without repeated retries");
  SetFileAttributesA(protectedDirectory+"\\Keep",FILE_ATTRIBUTE_NORMAL);
  CString driveRoot=root.Left(3);check(!DeleteHostSelectionJob(hostPath(driveRoot),true,MacBrowser::DescribeWindowsFile(hostPath(protectedDirectory)).identity,nullptr),"drive-root deletion accepted");
  CString ordinary=root+"\\Ordinary",unrelated=root+"\\%Ordinary";
  save(ordinary,"SELECTED");save(unrelated,"UNRELATED");check(removeHost(ordinary,false)!=0,"delete ordinary file");
  check(load(unrelated)==std::vector<BYTE>({'U','N','R','E','L','A','T','E','D'}),"deletion removed an unrelated percent-prefixed file");
  CString reverse=root+"\\Reverse",reverseMetadata=root+"\\%Reverse";save(reverse,"KEEP");save(reverseMetadata,"SELECTED");
  check(removeHost(reverseMetadata,false)!=0&&load(reverse)==std::vector<BYTE>({'K','E','E','P'}),"metadata selection removed unselected data file");
  std::vector<BYTE> appleDouble(70,0);appleDouble[1]=5;appleDouble[2]=0x16;appleDouble[3]=7;appleDouble[5]=2;appleDouble[25]=1;appleDouble[29]=9;appleDouble[33]=38;appleDouble[37]=32;
  CString paired=root+"\\Paired",sidecar=root+"\\%Paired";save(paired,"DATA");
  {CFile f;check(f.Open(sidecar,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create genuine AppleDouble");f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  check(removeHost(paired,false)!=0&&GetFileAttributesA(sidecar)==INVALID_FILE_ATTRIBUTES,"verified AppleDouble companion not deleted");
  save(paired,"KEEP");{CFile f;check(f.Open(sidecar,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create protected pair");f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  check(SetFileAttributesA(paired,FILE_ATTRIBUTE_READONLY)!=FALSE,"protect selected data file");
  check(!removeHost(paired,false)&&load(sidecar)==appleDouble,"failed data deletion removed its companion");SetFileAttributesA(paired,FILE_ATTRIBUTE_NORMAL);
  const auto expectedPair=MacBrowser::DescribeWindowsFile(hostPath(paired)).identity;
  const CString oldSidecar=sidecar+".original";
  check(MoveFileA(sidecar,oldSidecar)!=FALSE,"move original sidecar aside");
  {CFile f;check(f.Open(sidecar,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create replacement AppleDouble");f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  check(!DeleteHostSelectionJob(hostPath(paired),false,expectedPair,nullptr)&&load(sidecar)==appleDouble&&
        load(paired)==std::vector<BYTE>({'K','E','E','P'}),"stale sidecar deleted a replacement or selected data");
  const auto expectedPrimary=MacBrowser::DescribeWindowsFile(hostPath(paired)).identity;
  const CString oldPrimary=paired+".original";
  check(MoveFileA(paired,oldPrimary)!=FALSE,"move selected Windows file aside");
  save(paired,"REPLACEMENT");
  check(!DeleteHostSelectionJob(hostPath(paired),false,expectedPrimary,nullptr)&&load(sidecar)==appleDouble&&
        load(paired)==std::vector<BYTE>({'R','E','P','L','A','C','E','M','E','N','T'}),"stale primary deleted a replacement or its sidecar");
  const auto expectedFolder=MacBrowser::DescribeWindowsFile(hostPath(protectedDirectory)).identity;
  check(MoveFileA(protectedDirectory,protectedDirectory+".original")&&CreateDirectoryA(protectedDirectory,nullptr),
        "replace selected Windows folder");
  save(protectedDirectory+"\\Keep","REPLACEMENT");
  check(!DeleteHostSelectionJob(hostPath(protectedDirectory),true,expectedFolder,nullptr)&&
        GetFileAttributesA(protectedDirectory+"\\Keep")!=INVALID_FILE_ATTRIBUTES,"stale folder deletion removed a replacement child");
  const CString nonJournaled=root+"\\NonJournaled",nested=nonJournaled+"\\Nested";
  check(CreateDirectoryA(nonJournaled,nullptr)&&CreateDirectoryA(nested,nullptr),
        "create fallback recursive AppleDouble fixture");
  save(nonJournaled+"\\Earlier","NEIGHBOR");
  save(nested+"\\Document","PRIMARY");
  {CFile f;check(f.Open(nested+"\\%Document",CFile::modeCreate|CFile::modeWrite)!=FALSE,
                "create nested AppleDouble companion");
   f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  const auto refused=removeHost(nonJournaled,true);
  const auto refusedError=GetLastError();
  if(refused||refusedError!=ERROR_NOT_SUPPORTED)
   throw std::runtime_error("fallback recursive Delete returned "+std::to_string(refused)+
                            " with Windows error "+std::to_string(refusedError)+" for a nested AppleDouble pair");
  check(load(nonJournaled+"\\Earlier")==std::vector<BYTE>({'N','E','I','G','H','B','O','R'})&&
        load(nested+"\\Document")==std::vector<BYTE>({'P','R','I','M','A','R','Y'})&&
        load(nested+"\\%Document")==appleDouble,"preflight modified files before finding the nested companion");
  save(nested+"\\%Document","UNRELATED");
  check(removeHost(nonJournaled,true)!=0,"fallback refused an unrelated percent-prefixed file");
  fprintf(report,"PASS: fallback recursive Delete refuses nested AppleDouble pairs before changing any entry and accepts ordinary percent-prefixed files.\n");
  fprintf(report,"PASS: Windows delete pins verified entries and refuses replaced primary files, folders and AppleDouble companions.\n");
  CString renameDirectory=root+"\\Rename",cwdDirectory=root+"\\Decoys";check(CreateDirectoryA(renameDirectory,nullptr)&&CreateDirectoryA(cwdDirectory,nullptr),"create rename fixtures");
  save(renameDirectory+"\\Before","SELECTED");save(cwdDirectory+"\\Before","DECOY");
  char cwd[MAX_PATH];check(GetCurrentDirectoryA(sizeof(cwd),cwd)!=0&&SetCurrentDirectoryA(cwdDirectory)!=FALSE,"set decoy working directory");
  char relativeOld[]="Before",relativeNew[]="After";
  check(!fat_rename(relativeOld,relativeNew)&&load(cwdDirectory+"\\Before")==std::vector<BYTE>({'D','E','C','O','Y'}),"relative rename touched unrelated working-directory entry");
  CString oldPath=renameDirectory+"\\Before",newPath=renameDirectory+"\\After";
  check(fat_rename(oldPath.GetBuffer(),newPath.GetBuffer())!=0,"absolute rename failed");oldPath.ReleaseBuffer();newPath.ReleaseBuffer();
  check(SetCurrentDirectoryA(cwd)!=FALSE,"restore working directory");
  check(load(newPath)==std::vector<BYTE>({'S','E','L','E','C','T','E','D'})&&GetFileAttributesA(cwdDirectory+"\\After")==INVALID_FILE_ATTRIBUTES,"rename escaped selected folder");
  save(oldPath,"OTHER");check(!fat_rename(newPath.GetBuffer(),oldPath.GetBuffer()),"rename overwrote existing destination");newPath.ReleaseBuffer();oldPath.ReleaseBuffer();
  for(const char* name : {"..","../Other","..\\Other","Other:Stream","Trailing.","Trailing "})check(!HfvValidRenameLeaf(name,false),"Windows rename accepts path/stream syntax");
  check(!HfvValidRenameLeaf("Parent:Other",true)&&HfvValidRenameLeaf("Valid name",true),"HFS rename leaf validation");
  fprintf(report,"PASS: Windows renames require absolute paths, preserve CWD decoys/collisions, and reject path or stream names.\n");
  fprintf(report,"PASS: companion deletion requires validated AppleDouble metadata and successful primary deletion; unrelated files are preserved.\n");
  fprintf(report,"PASS: Windows recursive deletion removes junctions without following them, handles dotfiles and stops on errors.\n");
  fprintf(report,"PASS: recursive HFS root deletion is rejected before any child mutation.\n");
 }catch(const std::exception& error){HFSIFACE_silencer(silent);fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}
 HFSIFACE_silencer(silent);fclose(report);return 0;
}
#endif
