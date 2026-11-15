// SPDX-License-Identifier: GPL-2.0-or-later
#include "FakeFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "Filehook.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "floppy.h"
}
static void check(bool ok,const char* action) {
    if(!ok){fprintf(stderr,"FAIL: floppy: %s (HFS=%s errno=%d Windows=%lu)\n",action,hfs_error?hfs_error:"",errno,GetLastError());exit(1);}
}
static std::vector<BYTE> load(const char* path) {
    FILE* file=fopen(path,"rb");check(file!=NULL,"open fixture");fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
    check(size>0,"fixture length");std::vector<BYTE> bytes(size);check(fread(bytes.data(),1,bytes.size(),file)==bytes.size(),"load fixture");fclose(file);return bytes;
}
static void originalCache(const std::vector<BYTE>& bytes) {
    FakeFloppy api(bytes);ReadOnlyFloppy disk(&api);BYTE data[512];
    check(disk.OpenDrive(0),"open original-cache fixture");
    std::vector<BYTE> batch(70001);unsigned checks=api.verifies;
    check(disk.ReadBytes(511,DWORD(batch.size()),batch.data())&&!memcmp(batch.data(),bytes.data()+511,batch.size()),"batched contiguous miss byte fidelity");
    check(api.reads==3&&api.verifies-checks==4,"one request media check plus post-transfer checks, without duplicate pre-checks");
    unsigned reads=api.reads;checks=api.verifies;
    for(unsigned i=0;i<100;++i)check(disk.ReadBytes(1024,512,data)&&!memcmp(data,bytes.data()+1024,512),"original cache repeated bytes");
    check(api.reads==reads&&api.verifies==checks+100&&api.opens==1,"100 repeated reads use cached sectors and reuse the handle, while still checking media");
    std::vector<BYTE> thousand(1000*512);
    check(disk.OpenDrive(0)&&disk.ReadBytes(0,DWORD(thousand.size()),thousand.data()),"fill original 1000-sector cache");
    reads=api.reads;check(disk.ReadBytes(999*512,512,data)&&api.reads==reads,"last cached sector retained");
    check(disk.ReadBytes(1000*512,512,data),"cache capacity overflow");
    reads=api.reads;check(disk.ReadBytes(0,512,data)&&api.reads==reads+1&&!memcmp(data,bytes.data(),512),"original replacement policy evicts old sector without losing bytes");
    ++api.changeCount;reads=api.reads;
    check(!disk.ReadBytes(0,512,data)&&disk.Error()==ERROR_MEDIA_CHANGED&&api.reads==reads,"changed disk cannot return cached bytes");
    --api.changeCount;check(disk.OpenDrive(0)&&disk.ReadBytes(0,512,data)&&api.reads==reads+1,"reopen clears cached sectors");
    api.readError=ERROR_CRC;check(!disk.ReadBytes(1400*512,512,data),"uncached read error invalidates session");
    check(!disk.ReadBytes(0,512,data),"cached data cannot hide prior I/O error");
    disk.Close();check(api.opens==api.closes&&api.media==bytes,"cache handles balanced and media unchanged");
    puts("PASS: original 1000-sector cache, contiguous read batching, cached repeat/overlap, exact media-check counts, handle reuse, eviction and cache invalidation on errors/media change/reopen");
}
static void transport(const std::vector<BYTE>& bytes) {
    FakeFloppy fake(bytes);ReadOnlyFloppy disk(&fake);BYTE data[70001];
    check(!disk.OpenDrive(-1) && !disk.OpenDrive(26) && fake.opens==0,"reject invalid drive indices before opening");
    check(disk.OpenDrive(25) && fake.devicePath=="\\\\.\\Z:" && fake.readOnly,"read-only native open contract at arbitrary drive letter");
    check(disk.FindHfs() && disk.VolumeSize()==bytes.size(),"detect classic HFS floppy");
    check(disk.ReadVolume(511,sizeof(data),data) && !memcmp(data,bytes.data()+511,sizeof(data)) && fake.aligned,"unaligned byte request uses aligned multi-transfer sectors");
    check(disk.ReadVolume(bytes.size(),0,NULL),"zero-byte EOF");unsigned reads=fake.reads;
    check(!disk.ReadVolume(bytes.size()-1,2,data) && disk.Error()==ERROR_HANDLE_EOF && fake.reads==reads,"bounded read does not touch device");
    check(!disk.ReadVolume(UINT64_MAX,1,data),"reject extreme offset");
    ++fake.changeCount;
    check(!disk.ReadVolume(1024,2,data) && disk.Error()==ERROR_MEDIA_CHANGED && fake.reads==reads,"media generation change rejects stale handle");
    --fake.changeCount;check(!disk.ReadVolume(0,1,data),"changed session remains invalid until reopen");
    check(disk.OpenDrive(0) && disk.FindHfs(),"reopen after media change");fake.shortRead=true;
    check(!disk.ReadVolume(0,1,data) && disk.Error()==ERROR_READ_FAULT,"short sector read is an error");disk.Close();
    check(fake.opens==fake.closes && fake.media==bytes,"transport handles closed and media unchanged");
    for(unsigned mode=0;mode<8;++mode) {
        FakeFloppy bad(bytes);ReadOnlyFloppy rejected(&bad);
        if(mode==0) bad.driveType=DRIVE_FIXED;
        if(mode==1) bad.geometry.MediaType=RemovableMedia;
        if(mode==2) bad.geometry.BytesPerSector=4096;
        if(mode==3) bad.geometry.Cylinders.QuadPart=INT64_MAX;
        if(mode==4) bad.geometry.SectorsPerTrack=9;
        if(mode==5) bad.openError=ERROR_ACCESS_DENIED;
        if(mode==6) bad.geometryError=ERROR_NOT_READY;
        if(mode==7) bad.verifyError=ERROR_NO_MEDIA_IN_DRIVE;
        check(!rejected.OpenDrive(0),"reject non-floppy geometry and device errors");
        check(bad.closes==bad.opens-(mode==5?1:0) && bad.reads==0,"failed open releases handles without sector reads");
    }
    for(MEDIA_TYPE type:{F3_720_512,F3_2Pt88_512}) {
        unsigned sectors=type==F3_720_512?9:36;FakeFloppy other(std::vector<BYTE>(80*2*sectors*512));
        other.geometry.MediaType=type;other.geometry.SectorsPerTrack=sectors;ReadOnlyFloppy disk2(&other);
        check(disk2.OpenDrive(1) && disk2.MediaSize()==other.media.size(),"supported 720 KiB/2.88 MB MFM geometry");
        check(!disk2.FindHfs(),"blank/non-HFS media does not become an HFS volume");
    }
    FakeFloppy io(bytes);ReadOnlyFloppy failing(&io);check(failing.OpenDrive(0),"open read-error fixture");io.readError=ERROR_CRC;
    check(!failing.FindHfs() && failing.Error()==ERROR_CRC,"sector errors are propagated without invented data");
    io.readError=0;check(!failing.ReadBytes(0,1,data),"failed session remains invalid");
    FakeFloppy swapped(bytes);ReadOnlyFloppy changing(&swapped);check(changing.OpenDrive(0),"open mid-read change fixture");
    swapped.changeAfterRead=1;data[0]=0xa5;
    check(!changing.ReadBytes(0,1,data) && changing.Error()==ERROR_MEDIA_CHANGED && data[0]==0xa5,"media change during sector read rejected before copying data");
    FakeFloppy malformed(bytes);malformed.media[1024+20]=0xff;ReadOnlyFloppy invalid(&malformed);
    check(invalid.OpenDrive(0) && !invalid.FindHfs() && invalid.Error()==ERROR_UNRECOGNIZED_VOLUME,"out-of-media HFS allocation layout rejected");
}
static void backend(const std::vector<BYTE>& bytes) {
    FakeFloppy fake(bytes);check(readonly_floppy_attach_api(25,&fake),"attach simulated device");readonly_cd_backend_init();
    char path[64];get_floppy_volume_file_name(25,path);check(readonly_media_path(path)!=0,"reserved read-only floppy path");
    check(fhook_open(path,O_RDWR)<0 && errno==EROFS && fake.opens==0,"reject writable open before touching device");
    check(fhook_open(path,O_CREAT|O_TRUNC|O_WRONLY)<0 && errno==EROFS,"reject create/truncate");
    char rejectedLabel[]="Forbidden";
    check(hfs_format(path,0,rejectedLabel)<0 && fake.opens==0,"format cannot open physical media for writing");
    int first=fhook_open(path,O_RDONLY),second=fhook_open(path,O_RDONLY);check(first>=0 && second>=0 && first!=second && fake.opens==1,"reference-counted independent descriptors");
    check(fhook_lseek(first,1024,SEEK_SET)==1024 && fhook_lseek(second,0,SEEK_CUR)==0,"independent seek positions");
    BYTE signature[2];check(fhook_read(first,signature,2)==2 && signature[0]==0x42 && signature[1]==0x44,"read HFS MDB through native backend");
    check(fhook_lseek(first,INT64_MAX,SEEK_CUR)<0 && fhook_lseek(first,INT64_MIN,SEEK_END)<0,"overflow-safe seeks");
    check(fhook_lseek(first,0,SEEK_END)==bytes.size() && fhook_read(first,signature,2)==0,"descriptor EOF clips reads");
    check(fhook_write(first,"X",1)<0 && errno==EROFS,"descriptor writes are rejected");
    check(floppy_write(flcds[25].handle,25,0,1,(char*)signature)==0 && GetLastError()==ERROR_WRITE_PROTECT,"legacy write entry point stays disabled");
    check(fhook_close(first)==0 && fake.closes==0 && fhook_close(second)==0 && fake.closes==1,"last descriptor closes device");
    check(fhook_read(second,signature,1)<0 && errno==EBADF,"closed descriptor cannot access CRT");
    hfsvol* volume=hfs_mount(path,0,O_RDONLY);check(volume!=NULL,"mount simulated 1.44 MB native floppy in production HFS core");
    hfsvolent info;check(hfs_vstat(volume,&info)==0 && (info.flags&HFS_ISLOCKED),"HFS reports write protection");
    char name[]="FloppyTest:Payload",denied[]="FloppyTest:Forbidden",type[]="TEXT",creator[]="ttxt";
    hfsfile* file=hfs_open(volume,name);check(file!=NULL,"open file from physical-sector adapter");BYTE payload[8193];
    check(hfs_read(file,payload,sizeof(payload))==sizeof(payload),"extract full data fork");
    for(unsigned i=0;i<sizeof(payload);++i) check(payload[i]==BYTE(i*37),"data fork byte fidelity");
    check(hfs_setfork(file,1)==0 && hfs_read(file,payload,1025)==1025,"extract resource fork");
    for(unsigned i=0;i<1025;++i) check(payload[i]==BYTE(i*71),"resource fork byte fidelity");
    check(hfs_write(file,payload,1)<0 && hfs_truncate(file,1)<0,"HFS fork writes/truncation rejected");
    check(hfs_create(volume,denied,type,creator)<0 && hfs_delete(volume,name)<0 && hfs_rename(volume,name,denied)<0 && hfs_mkdir(volume,denied)<0,"HFS mutation operations rejected");
    hfs_close(file);check(hfs_umount(volume)==0,"unmount read-only floppy");
    first=fhook_open(path,O_RDONLY);check(first>=0,"reopen for media-change descriptor test");++fake.changeCount;
    check(fhook_read(first,signature,2)<0 && errno==EIO,"media-change error reaches file hooks");fhook_close(first);
    readonly_cd_backend_final();check(fake.opens==fake.closes && fake.media==bytes && fake.aligned && fake.readOnly,"backend never changed media or issued unaligned/writable I/O");
    check(readonly_floppy_attach_api(25,NULL),"detach fake before its lifetime ends");
}
int main(int argc,char** argv) {
    check(argc==2,"Usage: floppy-readonly NEW_TEST_DIRECTORY");check(CreateDirectoryA(argv[1],NULL)!=FALSE,"exclusive fixture directory");
    std::string path=std::string(argv[1])+"\\floppy-hfs.dsk";
    HANDLE file=CreateFileA(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);check(file!=INVALID_HANDLE_VALUE,"create 1.44 MB fixture");
    LARGE_INTEGER size;size.QuadPart=1474560;check(SetFilePointerEx(file,size,NULL,FILE_BEGIN)&&SetEndOfFile(file),"size fixture");CloseHandle(file);
    char label[]="FloppyTest",name[]="FloppyTest:Payload",type[]="TEXT",creator[]="ttxt";
    check(hfs_format(const_cast<char*>(path.c_str()),0,label)==0,"format generated floppy image");
    hfsvol* volume=hfs_mount(const_cast<char*>(path.c_str()),0,O_RDWR);check(volume!=NULL,"populate fixture");check(hfs_create(volume,name,type,creator)==0,"create payload");
    hfsfile* payload=hfs_open(volume,name);check(payload!=NULL,"open fixture payload");BYTE bytes[8193];
    for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=BYTE(i*37);check(hfs_write(payload,bytes,sizeof(bytes))==sizeof(bytes),"write fixture data fork");
    check(hfs_setfork(payload,1)==0,"select fixture resource fork");for(unsigned i=0;i<1025;++i) bytes[i]=BYTE(i*71);
    check(hfs_write(payload,bytes,1025)==1025 && hfs_close(payload)==0 && hfs_umount(volume)==0,"persist fixture");
    auto original=load(path.c_str());transport(original);originalCache(original);backend(original);check(load(path.c_str())==original,"fixture file unchanged by native-read tests");
    puts("PASS: native read-only floppy: open access/geometry, aligned sector I/O, bounds/EOF, device and media-change errors, descriptor isolation, HFS fork extraction, mutation rejection and unchanged media; all devices simulated");return 0;
}
