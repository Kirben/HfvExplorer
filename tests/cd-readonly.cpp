// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include <string>
#include "ReadOnlyCd.h"
#include "ReadOnlyCdBackend.h"
#include "Filehook.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "floppy.h"
}
static void check(bool ok,const char* label) { if(!ok) { fprintf(stderr,"FAIL: %s (HFS: %s; errno %d)\n",label,hfs_error,errno); exit(1); } }
static std::vector<BYTE> load(const char* path) {
    FILE* f=fopen(path,"rb"); check(f!=NULL,"open fixture file");
    fseek(f,0,SEEK_END); long bytes=ftell(f); rewind(f);
    check(bytes>0,"fixture size"); std::vector<BYTE> v(bytes);
    check(fread(v.data(),1,v.size(),f)==v.size(),"read fixture"); fclose(f); return v;
}
static void be16(BYTE* p,WORD n) { p[0]=(BYTE)(n>>8); p[1]=(BYTE)n; }
static void be32(BYTE* p,DWORD n) { p[0]=(BYTE)(n>>24); p[1]=(BYTE)(n>>16); p[2]=(BYTE)(n>>8); p[3]=(BYTE)n; }
static void save(const std::string& path,const std::vector<BYTE>& bytes) {
    int fd=_open(path.c_str(),_O_BINARY|_O_CREAT|_O_EXCL|_O_WRONLY,_S_IREAD|_S_IWRITE);
    check(fd>=0,"exclusive fixture creation"); check(_write(fd,bytes.data(),(unsigned)bytes.size())==(int)bytes.size(),"save fixture"); _close(fd);
}
static void originalCache(const std::string& root){
    std::vector<BYTE> bytes(1200*2048);for(size_t n=0;n<bytes.size();++n)bytes[n]=BYTE(n*37+n/2048);
    auto path=root+"cache.iso";save(path,bytes);ReadOnlyCd cd;BYTE out[7000];
    check(cd.OpenFixture(path.c_str())&&cd.ReadBytes(37,100,out)&&!memcmp(out,bytes.data()+37,100),"cache first partial sector fidelity");
    check(cd.test.reads==1&&cd.test.bytes==65536&&NBLOCKS==1000,"original 32-sector read-ahead and 1000 cache slots");
    for(unsigned n=0;n<100;++n)check(cd.ReadBytes(2048+n,100,out)&&!memcmp(out,bytes.data()+2048+n,100),"read-ahead cache fidelity");
    check(cd.test.reads==1&&cd.test.checks==102,"100 repeated reads avoid sector I/O and retain identity checks");
    check(cd.ReadBytes(2047,sizeof(out),out)&&!memcmp(out,bytes.data()+2047,sizeof(out))&&cd.test.reads==1,"overlapping unaligned cached sectors");
    ++cd.test.generation;unsigned reads=cd.test.reads;out[0]=0xa5;
    check(!cd.ReadBytes(0,1,out)&&cd.Error()==ERROR_MEDIA_CHANGED&&cd.test.reads==reads&&out[0]==0xa5,"changed media cannot return cached bytes");
    --cd.test.generation;check(!cd.ReadBytes(0,1,out),"media-change session invalid until reopen");
    check(cd.OpenFixture(path.c_str()),"cache reopen");std::vector<BYTE> whole(bytes.size());
    check(cd.ReadBytes(0,DWORD(whole.size()),whole.data())&&whole==bytes,"multi-transfer cached read fidelity");reads=cd.test.reads;
    check(cd.ReadBytes(0,1,out)&&cd.test.reads==reads+1&&out[0]==bytes[0],"bounded cache eviction refetches old sectors");
    check(cd.OpenFixture(path.c_str())&&cd.ReadBytes(bytes.size()-1,1,out)&&out[0]==bytes.back()&&cd.test.bytes==2048,"read-ahead clipped at real EOF");
    check(!cd.ReadBytes(bytes.size(),1,out),"cache never extends capacity");
    for(int fault=0;fault<3;++fault){
        check(cd.OpenFixture(path.c_str()),"open cache error fixture");out[0]=0xa5;
        if(fault==0)cd.test.readError=ERROR_CRC;if(fault==1)cd.test.shortRead=true;if(fault==2)cd.test.changeAfterRead=1;
        check(!cd.ReadBytes(0,1,out)&&out[0]==0xa5,"error or changed transfer not returned/cached");
        check(cd.Error()==(fault==0?ERROR_CRC:fault==1?ERROR_READ_FAULT:ERROR_MEDIA_CHANGED),"precise cached read failure");
        cd.test.readError=0;cd.test.shortRead=false;check(!cd.ReadBytes(0,1,out),"failed session cannot return stale bytes");
    }
    check(cd.OpenFixture(path.c_str()),"open speculative read fallback");cd.test.failAbove=2048;
    check(cd.ReadBytes(37,100,out)&&!memcmp(out,bytes.data()+37,100)&&cd.test.reads==2,"failed speculative read retries exact requested sector");reads=cd.test.reads;
    check(cd.ReadBytes(0,1,out)&&cd.test.reads==reads,"successful exact retry is cached");
    check(cd.OpenFixture(path.c_str()),"open missing identity fixture");cd.test.noIdentity=true;
    check(cd.ReadBytes(0,1,out)&&cd.ReadBytes(0,1,out)&&cd.test.reads==2&&cd.test.bytes==4096,"unavailable media counter disables caching and speculative read-ahead");
    check(cd.OpenFixture(path.c_str())&&cd.ReadBytes(0,1,out),"establish cached identity before losing counter");cd.test.noIdentity=true;out[0]=0xa5;
    check(!cd.ReadBytes(0,1,out)&&cd.Error()==ERROR_NOT_SUPPORTED&&out[0]==0xa5,"loss of established identity invalidates cache/session");
    cd.Close();check(load(path.c_str())==bytes,"cache leaves source unchanged");
    puts("PASS: original CD 1000-sector cache and 32-sector read-ahead, repeated/overlap reads, eviction, EOF clipping, generation changes, read/short-read faults, exact-read fallback, reopen invalidation and unsupported identity fallback; fixture files only");
}
static void exercise(const std::string& path,const std::vector<BYTE>& original,DWORD start,DWORD length) {
    ReadOnlyCd cd;
    check(cd.OpenFixture(path.c_str()),"open read-only sector fixture");
    check(cd.FindHfs() && cd.VolumeOffset()==start && cd.VolumeSize()==length,"locate HFS volume");
    std::vector<BYTE> data(131071);
    check(cd.ReadVolume(1023,(DWORD)data.size(),data.data()) && !memcmp(data.data(),original.data()+1023,data.size()),"unaligned multi-chunk sector reads");
    check(!cd.ReadVolume(length-1,2,data.data()),"reject out-of-range reads");
    check(cd.ReadVolume(length,0,NULL),"zero-byte EOF read"); cd.Close();
    check(readonly_cd_attach_fixture(25,path.c_str()),"attach fixture to CD backend");
    readonly_cd_backend_init();
    char pseudo[64]; get_cd_volume_file_name(25,pseudo);
    check(fhook_open(pseudo,O_RDWR)<0 && errno==EROFS,"reject read/write open");
    check(fhook_open(pseudo,O_WRONLY|O_TRUNC)<0 && errno==EROFS,"reject truncate open");
    int fd=fhook_open(pseudo,O_RDONLY); check(fd>=0,"open read-only CD descriptor");
    check(fhook_write(fd,"X",1)<0 && errno==EROFS,"reject descriptor writes");
    check(fhook_lseek(fd,1024,SEEK_SET)==1024,"seek CD descriptor");
    BYTE sig[2]; check(fhook_read(fd,sig,2)==2 && sig[0]==0x42 && sig[1]==0x44,"read MDB through hooks");
    int second=fhook_open(pseudo,O_RDONLY); check(second>=0 && second!=fd,"independent CD descriptors");
    check(fhook_lseek(second,0,SEEK_CUR)==0 && fhook_lseek(fd,0,SEEK_CUR)==1026,"independent positions");
    check(fhook_close(second)==0 && fhook_close(fd)==0,"close reference-counted descriptors");
    hfsvol* volume=hfs_mount(pseudo,0,O_RDONLY); check(volume!=NULL,"mount CD through actual HFS core");
    char file[]="X64Test:Folder:Renamed", type[]="TEXT", creator[]="ttxt", forbidden[]="X64Test:Forbidden";
    hfsfile* h=hfs_open(volume,file); check(h!=NULL,"open file on read-only CD");
    BYTE bytes[8193]; check(hfs_read(h,bytes,sizeof(bytes))==sizeof(bytes),"extract data fork");
    for(unsigned n=0;n<sizeof(bytes);++n) check(bytes[n]==(BYTE)(n*37),"data fork byte fidelity");
    check(hfs_setfork(h,1)==0 && hfs_read(h,bytes,1025)==1025,"extract resource fork");
    for(unsigned n=0;n<1025;++n) check(bytes[n]==(BYTE)(n*71),"resource fork byte fidelity");
    check(hfs_write(h,"X",1)<0,"HFS rejects file writes"); hfs_close(h);
    check(hfs_create(volume,forbidden,type,creator)<0,"HFS rejects create");
    check(hfs_delete(volume,file)<0,"HFS rejects delete");
    check(hfs_umount(volume)==0,"unmount CD core");
    // Writable images must still retain the CRT creation mode and read/write path.
    std::string normal=path+".normal";
    fd=fhook_open(normal.c_str(),_O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY,_S_IREAD|_S_IWRITE);
    check(fd>=0 && fhook_write(fd,"image",5)==5 && fhook_lseek(fd,0,SEEK_SET)==0,"ordinary writable file passthrough");
    check(fhook_read(fd,bytes,5)==5 && !memcmp(bytes,"image",5),"ordinary image readback"); fhook_close(fd);
    readonly_cd_backend_final();
    printf("PASS: %s\n",path.c_str());
}
int main(int argc,char** argv) {
    if(argc!=3) { puts("Usage: cd-readonly.exe POPULATED_HFS_IMAGE NEW_FIXTURE_DIRECTORY"); return 2; }
    check(CreateDirectoryA(argv[2],NULL)!=0,"exclusive test directory creation");
    std::vector<BYTE> raw=load(argv[1]);
    std::string root=std::string(argv[2])+"\\";
    originalCache(root);
    std::string plain=root+"plain.iso"; save(plain,raw); exercise(plain,raw,0,(DWORD)raw.size());
    check(load(plain.c_str())==raw,"plain fixture unchanged");
    for(DWORD blockSize=512;blockSize<=2048;blockSize*=4) {
        DWORD start=65536;
        std::vector<BYTE> hybrid(raw.size()+start,0);
        memcpy(hybrid.data()+start,raw.data(),raw.size());
        be16(hybrid.data(),0x4552); be16(hybrid.data()+2,(WORD)blockSize);
        BYTE* map=hybrid.data()+blockSize;
        be16(map,0x504d); be32(map+4,1); be32(map+8,start/blockSize); be32(map+12,(DWORD)raw.size()/blockSize);
        memcpy(map+48,"Apple_HFS",9);
        std::string path=root+(blockSize==512?"hybrid512.iso":"hybrid2048.iso");
        save(path,hybrid); exercise(path,raw,start,(DWORD)raw.size());
        {
            ReadOnlyCd shortExtent;
            check(shortExtent.OpenFixture(path.c_str(),32768),"simulate undersized Windows length query");
            check(shortExtent.LengthQuerySize()==32768 && shortExtent.MediaSize()==hybrid.size(),"prefer full CD TOC capacity");
            check(shortExtent.FindHfs() && shortExtent.VolumeOffset()==start,"mount HFS beyond reported Windows extent");
            BYTE tail[512];
            check(shortExtent.ReadVolume(raw.size()-sizeof(tail),sizeof(tail),tail) &&
                  !memcmp(tail,raw.data()+raw.size()-sizeof(tail),sizeof(tail)),"read partition tail using full media capacity");
            check(!shortExtent.ReadVolume(raw.size(),1,tail),"retain real end-of-media bounds after extent correction");
        }
        check(load(path.c_str())==hybrid,"partitioned fixture unchanged");
        be32(map+4,0xffffffff); std::string bad=path+".bad"; save(bad,hybrid);
        ReadOnlyCd cd; check(cd.OpenFixture(bad.c_str()) && !cd.FindHfs(),"reject malformed map count");
    }
    std::vector<BYTE> displaced(raw.size()+65536,0);
    memcpy(displaced.data()+65536,raw.data(),raw.size());
    std::string displacedPath=root+"mapless-hybrid.iso";
    save(displacedPath,displaced); exercise(displacedPath,raw,65536,(DWORD)raw.size());
    check(load(displacedPath.c_str())==displaced,"mapless fixture unchanged");
    // Genuine HFS geometry is retained while trailing media is removed, as on
    // the user's CD. Existing catalog/file extents remain in the retained half.
    {
        DWORD start=65536, kept=(DWORD)raw.size()/2;
        std::vector<BYTE> shortened(start+kept,0);
        memcpy(shortened.data()+start,raw.data(),kept);
        be16(shortened.data(),0x4552); be16(shortened.data()+2,512);
        be32(shortened.data()+4,(DWORD)(start+raw.size())/512);
        BYTE* map=shortened.data()+512;
        be16(map,0x504d); be32(map+4,1); be32(map+8,start/512); be32(map+12,(DWORD)raw.size()/512);
        memcpy(map+48,"Apple_HFS",9);
        std::string shortPath=root+"shortened.iso"; save(shortPath,shortened);
        exercise(shortPath,raw,start,kept);
        ReadOnlyCd shortCd;
        check(shortCd.OpenFixture(shortPath.c_str()) && shortCd.FindHfs(),"recognize shortened HFS partition");
        check(shortCd.DeclaredVolumeSize()==raw.size() && shortCd.VolumeSize()==kept,"separate declared and readable volume sizes");
        BYTE untouched=0xa5;
        check(!shortCd.ReadVolume(kept,1,&untouched) && untouched==0xa5,"missing sectors fail without zero fill");
        check(load(shortPath.c_str())==shortened,"shortened media unchanged after extraction");
        // A clipped partition with catalog extents outside real media cannot
        // provide a usable filesystem and must remain rejected.
        be16(shortened.data()+start+1024+150,0xfffe);
        be16(shortened.data()+start+1024+152,1);
        std::string badCatalog=root+"missing-catalog.iso"; save(badCatalog,shortened);
        ReadOnlyCd missing;
        check(missing.OpenFixture(badCatalog.c_str()) && !missing.FindHfs(),"reject missing catalog extent");
    }
    std::vector<BYTE> nonHfs(65536,0); std::string non=root+"nonhfs.iso"; save(non,nonHfs);
    ReadOnlyCd cd; check(cd.OpenFixture(non.c_str()) && !cd.FindHfs(),"reject non-HFS media");
    puts("PASS: synthetic CDs, partition maps, extraction, EOF bounds, independent seeks, image writes, CD write rejection, unchanged media");
    return 0;
}
