// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "Mactypes.h"
#include "LargeForkTestData.h"
#include "adouble.h"
#include <winioctl.h>
#include <fcntl.h>
#include <stdio.h>
extern "C" {
#include "hfs/ClassicHfs.h"
#include "hfs/copyin.h"
}

static void check(bool ok,const char* message) {if(!ok) throw message;}
static void store32(BYTE* p,unsigned long v) {p[0]=BYTE(v>>24);p[1]=BYTE(v>>16);p[2]=BYTE(v>>8);p[3]=BYTE(v);}
static void testHighAppleDouble(const CString& directory) {
 CString resource=directory+"high-offset.ad",data=directory+"small-data",destination=directory+"ad-test.dsk";
 HANDLE out=CreateFileA(resource,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);
 check(out!=INVALID_HANDLE_VALUE,"create sparse high-offset AppleDouble fixture");DWORD ignored;
 check(DeviceIoControl(out,FSCTL_SET_SPARSE,NULL,0,NULL,0,&ignored,NULL)!=FALSE,"sparse AppleDouble fixture");
 LARGE_INTEGER position;position.QuadPart=0x80002080ULL;
 check(SetFilePointerEx(out,position,NULL,FILE_BEGIN)&&SetEndOfFile(out),"size high-offset AppleDouble");
 BYTE header[50]={};store32(header,0x00051607);store32(header+4,0x00020000);header[25]=2;
 store32(header+26,2);store32(header+30,0x80002000UL);store32(header+34,128);
 store32(header+38,9);store32(header+42,0x80001000UL);store32(header+46,16);
 DWORD written;position.QuadPart=0;
 check(SetFilePointerEx(out,position,NULL,FILE_BEGIN)&&WriteFile(out,header,sizeof(header),&written,NULL)&&written==sizeof(header),"write AppleDouble header");
 BYTE finder[16]={};memcpy(finder,"TEXTttxt",8);position.QuadPart=0x80001000ULL;
 check(SetFilePointerEx(out,position,NULL,FILE_BEGIN)&&WriteFile(out,finder,16,&written,NULL)&&written==16,"write high Finder information");
 BYTE bytes[128];for(unsigned i=0;i<128;++i) bytes[i]=LargeImagePattern(i,118);
 position.QuadPart=0x80002000ULL;
 check(SetFilePointerEx(out,position,NULL,FILE_BEGIN)&&WriteFile(out,bytes,128,&written,NULL)&&written==128,"write high resource data");CloseHandle(out);
 CFile file;check(file.Open(data,CFile::modeCreate|CFile::modeWrite|CFile::shareExclusive)!=FALSE,"create small data fixture");file.Write("HELLO",5);file.Close();
 check(file.Open(destination,CFile::modeCreate|CFile::modeReadWrite|CFile::shareExclusive)!=FALSE,"create AppleDouble target image");file.SetLength(16*1024*1024);file.Close();
 char label[]="ADTest",target[]="ADTest:Imported",type[5],creator[5];unsigned long start,length;
 check(get_apple_double_type_creator(resource.GetBuffer(),type,creator,&start,&length)!=0 &&
       start==0x80002000UL && length==128 && !strcmp(type,"TEXT") && !strcmp(creator,"ttxt"),"production AppleDouble metadata above 2 GiB");resource.ReleaseBuffer();
 check(hfs_format(destination.GetBuffer(),0,label)==0,"format AppleDouble target");destination.ReleaseBuffer();
 hfsvol* volume=hfs_mount(destination.GetBuffer(),0,O_RDWR);destination.ReleaseBuffer();check(volume!=NULL,"mount AppleDouble target");
 check(cpi_double(data.GetBuffer(),resource.GetBuffer(),volume,target)==0,"production AppleDouble import uses 64-bit resource seek");data.ReleaseBuffer();resource.ReleaseBuffer();
 hfsfile* imported=hfs_open(volume,target);check(imported!=NULL,"open high-offset AppleDouble import");
 BYTE actual[128];check(hfs_read(imported,actual,128)==5 && !memcmp(actual,"HELLO",5),"imported data fork bytes");
 check(hfs_setfork(imported,1)==0 && hfs_read(imported,actual,128)==128 && !memcmp(actual,bytes,128),"imported high-offset resource bytes");
 check(hfs_close(imported)==0 && hfs_umount(volume)==0,"close high-offset AppleDouble test");
}
void RunLargeForkGuiTests(const char* metadata,FILE* report) {
 CString directory=metadata;directory=directory.Left(directory.ReverseFind('\\')+1);
 LargeForkManifest m={};FILE* source=fopen(directory+"large-forks.meta","rb");check(source!=NULL,"open large-fork manifest");
 size_t n=fread(&m,sizeof(m),1,source);fclose(source);check(n==1 && m.length==0x7fffffffUL,"validate Apple-maximum fork fixture");
 CHFVVolume volume;volume.m_file_name=m.image;check(volume.open()!=0,"GUI opens volume with both large forks");
 for(unsigned fork=0;fork<2;++fork){CatDataRec oversized={};oversized.u.f.filLgLen=fork?0:0x80000000UL;oversized.u.f.filRLgLen=fork?0x80000000UL:0;CHFVFile file;check(file.open_by_CDR(&volume,&oversized,fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)==0,"GUI rejects classic fork above Apple maximum");}
 CatDataRec entry={};entry.u.f.filFlNum=m.fileID;entry.u.f.filLgLen=entry.u.f.filRLgLen=m.length;
 for(unsigned i=0;i<3;++i) {
  entry.u.f.filExtRec[i].xdrStABN=m.data[i].start;entry.u.f.filExtRec[i].xdrNumABlks=m.data[i].count;
  entry.u.f.filRExtRec[i].xdrStABN=m.resource[i].start;entry.u.f.filRExtRec[i].xdrNumABlks=m.resource[i].count;
 }
 const hfv_offset_t offsets[]={0,0x7fffffe0LL,hfv_offset_t(m.length)-137,hfv_offset_t(m.length)-37};
 for(unsigned fork=0;fork<2;++fork) {
  CHFVFile file;check(file.open_by_CDR(&volume,&entry,fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)!=0,"GUI opens Apple-maximum fork");
  BYTE bytes[128];
  for(hfv_offset_t offset:offsets) {
   check(file.Seek(offset,CFile::begin)==offset,"GUI seek past signed fork limit");
   UINT expected=(UINT)min(hfv_offset_t(sizeof(bytes)),hfv_offset_t(m.length)-offset);
   check(file.Read(bytes,sizeof(bytes))==expected,"GUI large-fork boundary read");
   for(unsigned i=0;i<expected;++i) check(bytes[i]==LargeImagePattern((uint32_t)(offset+i),17+fork*101),"GUI large-fork payload");
  }
  check(file.Seek(-37,CFile::end)==m.length-37 && file.Read(bytes,0xffffffffU)==37,"GUI unsigned read count clips at large EOF");
  check(file.Seek(INT64_MAX,CFile::current)==-1,"GUI relative seek overflow rejected");
  check(file.Seek(0,CFile::end)==m.length && file.Read(bytes,1)==0,"GUI large-fork EOF");
 }
 volume.close();testHighAppleDouble(directory);
 fprintf(report,"PASS: GUI classic forks at Apple maximum: both forks, oversized-fork rejection, boundary/tail reads, EOF, extreme seek rejection, production AppleDouble metadata/import at host offsets above 2 GiB\n");
}
#endif
