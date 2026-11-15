// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "Mactypes.h"
#include "FakeFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "floppy.h"
#include <fcntl.h>
extern "C" int mount_flags(char* path);
#include <stdio.h>

static void require(bool ok,const char* action) {if(!ok) throw action;}
int RunFloppyGuiTest(const char* fixture,const char* reportPath) {
 HANDLE created=CreateFileA(reportPath,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,0,NULL);
 if(created==INVALID_HANDLE_VALUE) return 1;CloseHandle(created);
 FILE* report=fopen(reportPath,"w");if(!report) return 1;
 try {
  CFile source;require(source.Open(fixture,CFile::modeRead|CFile::shareDenyWrite)!=FALSE,"open generated fixture");
  std::vector<BYTE> bytes((size_t)source.GetLength());require(source.Read(bytes.data(),(UINT)bytes.size())==bytes.size(),"load generated fixture");source.Close();
  FakeFloppy fake(bytes);require(readonly_floppy_attach_api(25,&fake),"attach GUI simulated floppy");readonly_cd_backend_init();
  char path[64];get_floppy_volume_file_name(25,path);
  require(mount_flags(path)==O_RDONLY,"GUI interface mounts native floppy read-only");
  CDSKFile file;require(!file.Open(path,CFile::modeReadWrite) && fake.opens==0,"GUI rejects writable open before device access");
  require(file.Open(path,CFile::modeRead|CFile::shareDenyNone)!=FALSE && file.GetLength()==bytes.size(),"GUI adapter opens native floppy");
  BYTE block[513];require(file.Seek(1023,CFile::begin)==1023 && file.Read(block,sizeof(block))==sizeof(block) &&
      !memcmp(block,bytes.data()+1023,sizeof(block)),"GUI aligned adapter byte fidelity");
  require(file.Seek(-1,CFile::end)==bytes.size()-1 && file.Read(block,sizeof(block))==1,"GUI physical EOF clipping");
  require(file.Seek(INT64_MAX,CFile::current)==ULONGLONG(-1),"GUI extreme physical seek rejection");
  bool rejected=false;try {file.Write("X",1);} catch(CFileException* e){rejected=e->m_cause==CFileException::accessDenied;e->Delete();}
  require(rejected,"GUI Write rejects native media");
  rejected=false;try {file.SetLength(0);} catch(CFileException* e){rejected=e->m_cause==CFileException::accessDenied;e->Delete();}
  require(rejected,"GUI SetLength rejects native media");file.Close();
  CHFVVolume volume;volume.m_file_name=path;require(volume.open()!=0,"GUI volume parser opens physical HFS adapter");
  require(volume.m_volume_name=="FloppyTest" && volume.m_length==bytes.size(),"GUI native volume identity");volume.close();
  require(file.Open(path,CFile::modeRead)!=FALSE,"reopen GUI adapter for removal test");++fake.changeCount;
  rejected=false;try {file.Read(block,1);} catch(CFileException* e){rejected=true;e->Delete();}
  require(rejected,"GUI reports changed media as an I/O error");file.Close();
  fake.changeAfterRead=fake.reads+2;
  CHFVVolume changed;changed.m_file_name=path;
  require(changed.open()==0,"media change during volume parsing fails the open and releases its handle");
  fake.changeAfterRead=0;
  readonly_cd_backend_final();require(fake.readOnly && fake.aligned && fake.opens==fake.closes && fake.media==bytes,"GUI adapter preserves media and closes handles");
  require(readonly_floppy_attach_api(25,NULL),"detach GUI fake");
  fprintf(report,"PASS: GUI native-floppy adapter: read-only mount/open, geometry-sized reads, EOF, extreme seeks, Write/SetLength rejection, HFS volume identity, media-change exception and unchanged simulated media\n");
 } catch(const char* action) {fprintf(report,"FAIL: %s\n",action);fclose(report);return 1;}
   catch(CException* error) {char text[512];error->GetErrorMessage(text,sizeof(text));fprintf(report,"FAIL: MFC %s\n",text);error->Delete();fclose(report);return 1;}
 fclose(report);return 0;
}
#endif
