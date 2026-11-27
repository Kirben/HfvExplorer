// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include "ReadOnlyFloppy.h"
#include "ReadOnlyCdBackend.h"
#include "floppy.h"
extern "C" {
#include "hfs/ClassicHfs.h"
}
static void error(const char* action,DWORD code) {
 char* message=NULL;FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,code,0,(char*)&message,0,NULL);
 fprintf(stderr,"%s: Windows error %lu: %s\n",action,code,message?message:"");if(message) LocalFree(message);
}
int main(int argc,char** argv) {
 if(argc!=2 || !isalpha((unsigned char)argv[1][0]) || (strcmp(argv[1]+1,":") && strcmp(argv[1]+1,":\\") && argv[1][1])) {
  puts("Usage: floppy-probe A:   (read-only; no format/write/eject commands)");return 2;
 }
 int drive=toupper((unsigned char)argv[1][0])-'A';ReadOnlyFloppy disk;
 if(!disk.OpenDrive(drive)) {error("Open supported native floppy",disk.Error());return 1;}
 const DISK_GEOMETRY& geometry=disk.MediaGeometry();
 printf("Drive %c: opened with GENERIC_READ; media bytes: %lu; geometry: %lld/%lu/%lu, sector bytes: %lu\n",'A'+drive,disk.MediaSize(),geometry.Cylinders.QuadPart,geometry.TracksPerCylinder,geometry.SectorsPerTrack,geometry.BytesPerSector);
 if(!disk.FindHfs()) {error("No supported classic HFS volume identified",disk.Error());puts("No disc writes performed. This alone does not establish corruption; blank, FAT and GCR media are not supported HFS inputs.");return 1;}
 disk.Close();floppy_module_global_init();char path[64];get_floppy_volume_file_name(drive,path);
 hfsvol* volume=hfs_mount(path,0,O_RDONLY);
 if(!volume) {fprintf(stderr,"HFS mount failed: %s\n",hfs_error);floppy_module_global_final();return 1;}
 hfsvolent info;hfs_vstat(volume,&info);printf("Volume: %s (read-only)\n",info.name);
 char root[64];sprintf(root,"%s:",info.name);hfsdir* directory=hfs_opendir(volume,root);
 if(!directory) {fprintf(stderr,"Root listing failed: %s\n",hfs_error);hfs_umount(volume);floppy_module_global_final();return 1;}
 hfsdirent entry;unsigned count=0;
 while(hfs_readdir(directory,&entry)==0) {printf("%s %s",entry.flags&HFS_ISDIR?"DIR ":"FILE",entry.name);if(!(entry.flags&HFS_ISDIR)) printf(" [data=%lu resource=%lu]",entry.u.file.dsize,entry.u.file.rsize);puts("");++count;}
 bool complete=errno==ENOENT;
 hfs_closedir(directory);hfs_umount(volume);floppy_module_global_final();
 if(!complete) {fprintf(stderr,"Root listing stopped with an I/O error.\n");return 1;}
 printf("PASS: native read-only HFS floppy mount and root listing (%u entries); no disc writes performed.\n",count);return 0;
}
