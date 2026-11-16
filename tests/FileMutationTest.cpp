// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include <cstdio>
#include <fcntl.h>
#include <vector>
#include <stdexcept>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
int do_delete_directory(hfsvol*,char*);
}
namespace {
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
  fprintf(report,"PASS: recursive HFS root deletion is rejected before any child mutation.\n");
 }catch(const std::exception& error){HFSIFACE_silencer(silent);fprintf(report,"FAIL: %s\n",error.what());fclose(report);return 1;}
 HFSIFACE_silencer(silent);fclose(report);return 0;
}
#endif
