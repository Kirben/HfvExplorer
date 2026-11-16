// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include <cstdio>
#include <fcntl.h>
#include <vector>
#include <stdexcept>
#include <winioctl.h>
#include "RenameName.h"
extern int fat_rename(char*,char*);
extern int DeleteFATDir(CString&);
extern int DeleteHostSelection(const CString&,bool);
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
int do_delete_directory(hfsvol*,char*);
}
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
}
int RunFileMutationTest(const char* directory) {
 if(!CreateDirectoryA(directory,nullptr))return 2;
 CString root=directory,image=root+"\\mutations.dsk";FILE* report=nullptr;fopen_s(&report,(root+"\\file-mutations-report.txt"),"w");if(!report)return 2;
 int silent=HFSIFACE_silencer(1);
 try {
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
  CString external=root+"\\Outside",selected=root+"\\Selected",link=selected+"\\Junction";
  check(CreateDirectoryA(external,nullptr)&&CreateDirectoryA(selected,nullptr),"create directory deletion fixtures");
  save(external+"\\Keep","OUTSIDE");save(selected+"\\.hidden","DELETE");
  check(junction(link,external),"create unprivileged directory junction fixture");
  check(DeleteFATDir(selected)!=0,"delete selected tree containing a junction");
  check(GetFileAttributesA(selected)==INVALID_FILE_ATTRIBUTES&&load(external+"\\Keep")==std::vector<BYTE>({'O','U','T','S','I','D','E'}),"recursive Windows deletion followed a junction outside its target");
  CString protectedDirectory=root+"\\Locked";check(CreateDirectoryA(protectedDirectory,nullptr)!=FALSE,"create locked deletion fixture");
  save(protectedDirectory+"\\Keep","LOCKED");check(SetFileAttributesA(protectedDirectory+"\\Keep",FILE_ATTRIBUTE_READONLY)!=FALSE,"protect deletion fixture");
  check(!DeleteFATDir(protectedDirectory)&&GetFileAttributesA(protectedDirectory+"\\Keep")!=INVALID_FILE_ATTRIBUTES,"protected child deletion must fail without repeated retries");
  SetFileAttributesA(protectedDirectory+"\\Keep",FILE_ATTRIBUTE_NORMAL);
  CString driveRoot=root.Left(3);check(!DeleteFATDir(driveRoot),"drive-root deletion accepted");
  CString ordinary=root+"\\Ordinary",unrelated=root+"\\%Ordinary";
  save(ordinary,"SELECTED");save(unrelated,"UNRELATED");check(DeleteHostSelection(ordinary,false)!=0,"delete ordinary file");
  check(load(unrelated)==std::vector<BYTE>({'U','N','R','E','L','A','T','E','D'}),"deletion removed an unrelated percent-prefixed file");
  CString reverse=root+"\\Reverse",reverseMetadata=root+"\\%Reverse";save(reverse,"KEEP");save(reverseMetadata,"SELECTED");
  check(DeleteHostSelection(reverseMetadata,false)!=0&&load(reverse)==std::vector<BYTE>({'K','E','E','P'}),"metadata selection removed unselected data file");
  std::vector<BYTE> appleDouble(70,0);appleDouble[1]=5;appleDouble[2]=0x16;appleDouble[3]=7;appleDouble[5]=2;appleDouble[25]=1;appleDouble[29]=9;appleDouble[33]=38;appleDouble[37]=32;
  CString paired=root+"\\Paired",sidecar=root+"\\%Paired";save(paired,"DATA");
  {CFile f;check(f.Open(sidecar,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create genuine AppleDouble");f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  check(DeleteHostSelection(paired,false)!=0&&GetFileAttributesA(sidecar)==INVALID_FILE_ATTRIBUTES,"verified AppleDouble companion not deleted");
  save(paired,"KEEP");{CFile f;check(f.Open(sidecar,CFile::modeCreate|CFile::modeWrite)!=FALSE,"create protected pair");f.Write(appleDouble.data(),UINT(appleDouble.size()));}
  check(SetFileAttributesA(paired,FILE_ATTRIBUTE_READONLY)!=FALSE,"protect selected data file");
  check(!DeleteHostSelection(paired,false)&&load(sidecar)==appleDouble,"failed data deletion removed its companion");SetFileAttributesA(paired,FILE_ATTRIBUTE_NORMAL);
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
