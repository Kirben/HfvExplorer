// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "ReadOnlyCd.h"
#include "ReadOnlyCdBackend.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "floppy.h"
}

static int fail(const char* operation, DWORD error) {
    char message[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, message, sizeof(message), NULL);
    fprintf(stderr,"%s failed: Windows error %lu: %s\n",operation,error,message);
    if(error==ERROR_ACCESS_DENIED) fprintf(stderr,"Try this command again in an Administrator PowerShell window.\n");
    return 1;
}
static unsigned be16(const BYTE* p) { return ((unsigned)p[0]<<8)|p[1]; }
static DWORD be32(const BYTE* p) { return ((DWORD)p[0]<<24)|((DWORD)p[1]<<16)|((DWORD)p[2]<<8)|p[3]; }
static void diagnose(ReadOnlyCd& cd) {
    printf("Diagnostic v4; read transport: %s\n",cd.Transport());
    printf("Capacity bytes: length=%llu TOC=%llu SCSI=%llu selected=%llu; SCSI capacity error=%lu\n",
           cd.LengthQuerySize(),cd.TocQuerySize(),cd.ScsiQuerySize(),cd.MediaSize(),cd.ScsiCapacityError());
    const ULONGLONG offsets[]={0,512,1024,2048,32768};
    BYTE b[512];
    for(unsigned n=0;n<ARRAYSIZE(offsets);++n) {
        if(!cd.ReadBytes(offsets[n],sizeof(b),b)) { printf("Read at %llu failed: %lu\n",offsets[n],cd.Error()); continue; }
        printf("Offset %llu:",offsets[n]);
        for(unsigned k=0;k<16;++k) printf(" %02X",b[k]); puts("");
        if(offsets[n]==32768 && !memcmp(b+1,"CD001",5)) puts("ISO9660 volume descriptor detected.");
        if(offsets[n]==0 && be16(b)==0x4552) printf("Apple driver descriptor: logical block size %u; declared bytes %llu\n",be16(b+2),(ULONGLONG)be32(b+4)*be16(b+2));
    }
    for(DWORD step=512;step<=2048;step*=4) {
        if(!cd.ReadBytes(step,sizeof(b),b) || be16(b)!=0x504d) continue;
        DWORD entries=be32(b+4);
        printf("Apple partition map at %lu; entries=%lu\n",step,entries);
        for(DWORD n=1;n<=entries && n<=16;++n) {
            if(!cd.ReadBytes((ULONGLONG)n*step,sizeof(b),b)) break;
            char type[33]; memcpy(type,b+48,32); type[32]=0;
            printf("  Entry %lu: signature=%04X start=%lu blocks=%lu type=%.32s\n",n,be16(b),be32(b+8),be32(b+12),type);
        }
    }
    BYTE scan[32768]; unsigned found=0;
    ULONGLONG end=cd.MediaSize()<1024*1024?cd.MediaSize():1024*1024;
    for(ULONGLONG base=0;base+sizeof(scan)<=end;base+=sizeof(scan)) {
        if(!cd.ReadBytes(base,sizeof(scan),scan)) { printf("Scan read at %llu failed: %lu\n",base,cd.Error()); break; }
        for(unsigned n=0;n+512<=sizeof(scan);n+=512) {
            BYTE* m=scan+n; unsigned sig=be16(m);
            if((sig==0x4244 || sig==0x482b || sig==0x4858) && found++<16) {
                printf("Volume signature %04X at byte %llu: allocation blocks=%u block bytes=%lu first=%u name length=%u\n",
                       sig,base+n,be16(m+18),be32(m+20),be16(m+28),m[36]);
                if(sig==0x4244)
                    printf("  Bitmap start=%u; extents tree bytes=%lu first extent=(%u,%u); catalog bytes=%lu first extent=(%u,%u)\n",
                           be16(m+14),be32(m+130),be16(m+134),be16(m+136),be32(m+146),be16(m+150),be16(m+152));
            }
        }
    }
    printf("Diagnostic completed; transport: %s; no disc writes performed.\n",cd.Transport());
}
int main(int argc,char** argv) {
    if(argc!=2 || !isalpha((unsigned char)argv[1][0]) || argv[1][1]!=':' ||
       (argv[1][2] && (argv[1][2]!='\\' || argv[1][3]))) {
        puts("Usage: cd-probe.exe D:\nReads only; no disc changes or tray operations."); return 2;
    }
    int drive=toupper((unsigned char)argv[1][0])-'A';
    ReadOnlyCd cd;
    if(!cd.OpenDrive(drive)) return fail("Open CD",cd.Error());
    printf("Drive %c: opened; media bytes: %llu\n",'A'+drive,cd.MediaSize());
    printf("Probe v4: length query=%llu TOC=%llu SCSI=%llu\n",cd.LengthQuerySize(),cd.TocQuerySize(),cd.ScsiQuerySize());
    if(!cd.FindHfs()) {
        DWORD error=cd.Error();
        if(error==ERROR_UNRECOGNIZED_VOLUME) puts("No supported classic HFS volume was identified. This does not establish disc corruption.");
        else fail("Find classic HFS volume",error);
        diagnose(cd); return 1;
    }
    printf("HFS offset: %llu; volume bytes: %lu; transport: %s\n",cd.VolumeOffset(),cd.VolumeSize(),cd.Transport());
    if(cd.DeclaredVolumeSize()>cd.VolumeSize())
        printf("NOTICE: shortened HFS partition: metadata declares %lu bytes; only %lu are readable. Missing sectors are not synthesized.\n",
               cd.DeclaredVolumeSize(),cd.VolumeSize());
    cd.Close();
    readonly_cd_backend_init();
    char path[64]; get_cd_volume_file_name(drive,path);
    hfsvol* volume=hfs_mount(path,0,O_RDONLY);
    if(!volume) { fprintf(stderr,"HFS mount failed: %s\n",hfs_error); readonly_cd_backend_final(); return 1; }
    hfsvolent info;
    if(hfs_vstat(volume,&info)==0) printf("Volume: %s\n",info.name);
    char root[] = ":";
    hfsdir* dir=hfs_opendir(volume,root);
    if(!dir) { fprintf(stderr,"Root directory failed: %s\n",hfs_error); hfs_umount(volume); readonly_cd_backend_final(); return 1; }
    hfsdirent entry; unsigned count=0;
    for(;;) {
        errno=0;
        if(hfs_readdir(dir,&entry)<0) break;
        if(entry.flags & HFS_ISDIR) printf("DIR  %s\n",entry.name);
        else printf("FILE %s [data=%lu resource=%lu]\n",entry.name,entry.u.file.dsize,entry.u.file.rsize);
        ++count;
    }
    if(errno!=ENOENT) {
        fprintf(stderr,"Directory listing stopped before completion: %s\n",hfs_error);
        hfs_closedir(dir); hfs_umount(volume); readonly_cd_backend_final(); return 1;
    }
    hfs_closedir(dir); hfs_umount(volume); readonly_cd_backend_final();
    printf("PASS: read-only HFS CD mount and root listing (%u entries).\n",count);
    return 0;
}
