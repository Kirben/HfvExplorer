// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "Filehook.h"
#include "LargeImageTestData.h"
void readonly_cd_backend_init(void);
void readonly_cd_backend_final(void);

static void check(int ok,const char* action) {
    if(!ok) { fprintf(stderr,"FAIL: %s (errno=%d; HFS=%s)\n",action,errno,hfs_error?hfs_error:""); exit(1); }
}
void CreateFragmentedFixture(const char* directory);
int main(int argc,char** argv) {
    const uint64_t gib=1024ULL*1024*1024;
    LargeImageManifest manifest = {0};
    char image[1024], meta[1024], volumeName[]="LargeTest";
    hfsvol* volume; hfsfile* file; hfsvolent info;
    HANDLE handle; LARGE_INTEGER size; DWORD ignored;
    unsigned c,n; int fd;
    if(argc!=2) { puts("Usage: large-image-core NEW_TEST_DIRECTORY"); return 2; }
    check(CreateDirectoryA(argv[1],NULL),"exclusive directory creation");
    check(strlen(argv[1])<900,"test path length");
    sprintf(image,"%s\\large.dsk",argv[1]); sprintf(meta,"%s\\manifest.bin",argv[1]);
    handle=CreateFileA(image,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);
    check(handle!=INVALID_HANDLE_VALUE,"exclusive sparse image creation");
    check(DeviceIoControl(handle,FSCTL_SET_SPARSE,NULL,0,NULL,0,&ignored,NULL),"enable sparse test file");
    size.QuadPart=6*gib;
    check(SetFilePointerEx(handle,size,NULL,FILE_BEGIN) && SetEndOfFile(handle),"size 6 GiB sparse HFS image");
    CloseHandle(handle);
    readonly_cd_backend_init(); // Exercise normal image passthrough with CD hooks installed.
    check(hfs_format(image,0,volumeName)==0,"format 6 GiB HFS image");
    volume=hfs_mount(image,0,O_RDWR); check(volume!=NULL,"mount 6 GiB image read/write");
    check(hfs_vstat(volume,&info)==0 && info.totbytes>4*gib,"64-bit volume byte counts");
    memcpy(manifest.magic,"HFVL64",7); strcpy(manifest.image,image); manifest.imageSize=6*gib;
    manifest.allocationSize=volume->mdb.drAlBlkSiz;
    manifest.allocationStart=volume->mdb.drAlBlSt;
    for(c=0;c<2;++c) {
        uint64_t target=(c?4*gib+32*1024*1024:2*gib);
        unsigned long cursor=(unsigned long)((target-(uint64_t)manifest.allocationStart*512)/manifest.allocationSize);
        char name[64], type[]="TEXT",creator[]="ttxt";
        LargeImageCase* item=&manifest.cases[c];
        strcpy(item->name,c?"Above4GiB":"Cross2GiB");
        item->dataSize=c?131073:400123; item->resourceSize=12345; item->seed=17+c*79;
        sprintf(name,"LargeTest:%s",item->name);
        volume->mdb.drAllocPtr=(unsigned short)cursor;
        volume->flags |= HFS_UPDATE_MDB;
        check(hfs_create(volume,name,type,creator)==0,"create high-allocation HFS file");
        file=hfs_open(volume,name); check(file!=NULL,"open high-allocation HFS file");
        for(n=0;n<2;++n) {
            unsigned length=n?item->resourceSize:item->dataSize;
            unsigned char* bytes=malloc(length); unsigned k;
            check(bytes!=NULL,"allocate payload");
            for(k=0;k<length;++k) bytes[k]=LargeImagePattern(k,item->seed+(n?101:0));
            check(hfs_setfork(file,n)==0 && hfs_write(file,bytes,length)==length,"write fork beyond signed 32-bit offsets");
            check(hfs_lseek(file,0,SEEK_SET)==0,"rewind high-allocation fork");
            check(hfs_read(file,bytes,length)==length,"read back high-allocation fork");
            for(k=0;k<length;++k) check(bytes[k]==LargeImagePattern(k,item->seed+(n?101:0)),"fork byte fidelity");
            free(bytes);
        }
        check(hfs_close(file)==0,"flush high-allocation file");
    }
    check(hfs_umount(volume)==0,"persist large HFS image");
    volume=hfs_mount(image,0,O_RDONLY); check(volume!=NULL,"reopen large image");
    for(c=0;c<2;++c) {
        LargeImageCase* item=&manifest.cases[c]; char name[64]; uint64_t physical;
        sprintf(name,"LargeTest:%s",item->name); file=hfs_open(volume,name);
        check(file!=NULL,"reopen high file");
        physical=(uint64_t)manifest.allocationStart*512+(uint64_t)file->cat.u.fil.filExtRec[0].xdrStABN*manifest.allocationSize;
        check(c?physical>4*gib:(physical<2*gib && physical+item->dataSize>2*gib),"allocation actually exercises 2/4 GiB boundary");
        printf("%s physical offset: %llu\n",item->name,physical);
        for(n=0;n<3;++n) {
            item->data[n].start=file->cat.u.fil.filExtRec[n].xdrStABN; item->data[n].count=file->cat.u.fil.filExtRec[n].xdrNumABlks;
            item->resource[n].start=file->cat.u.fil.filRExtRec[n].xdrStABN; item->resource[n].count=file->cat.u.fil.filRExtRec[n].xdrNumABlks;
        }
        for(n=0;n<2;++n) {
            unsigned length=n?item->resourceSize:item->dataSize,k;
            unsigned char* bytes=malloc(length); check(bytes!=NULL,"allocate reopened fork");
            check(hfs_setfork(file,n)==0 && hfs_read(file,bytes,length)==length,"read persisted high fork");
            for(k=0;k<length;++k) check(bytes[k]==LargeImagePattern(k,item->seed+(n?101:0)),"persisted fork fidelity");
            free(bytes);
        }
        hfs_close(file);
    }
    check(hfs_umount(volume)==0,"close read-only large image");
    fd=fhook_open(image,_O_RDONLY|_O_BINARY); check(fd>=0,"open large host image through hooks");
    check(fhook_lseek(fd,0,SEEK_END)==(hfv_offset_t)(6*gib),"64-bit host seek end");
    { struct _stat64 st; check(fhook_stat(image,&st)==0 && st.st_size==6*gib,"64-bit host stat"); }
    fhook_close(fd);
    CreateFragmentedFixture(argv[1]);
    readonly_cd_backend_final();
    { FILE* out=fopen(meta,"wb"); check(out!=NULL,"create GUI test manifest"); check(fwrite(&manifest,sizeof(manifest),1,out)==1,"save test manifest"); fclose(out); }
    puts("PASS: sparse 6 GiB HFS format, crossing 2 GiB and above 4 GiB writes, both forks, reopen, byte fidelity, 64-bit seek/stat");
    return 0;
}
