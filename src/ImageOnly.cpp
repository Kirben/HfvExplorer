// Legacy floppy/hard-disk stubs; CDs use the read-only Windows backend.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "floppy.h"
#include "filehook.h"
#include "ReadOnlyCdBackend.h"
#include <stdio.h>
#include <sys/stat.h>

extern "C" {
flcd_device_type flcds[MAX_DEVICES] = {};
void floppy_module_global_init(void) { readonly_cd_backend_init(); }
void floppy_module_global_final(void) { readonly_cd_backend_final(); }
void set_patch_option(int) {}
void cd_log_enable(int, char*) {}
void cd_log_restore_enable(int, char*) {}
void clear_cache_by_drive_index(int) {}
BOOL writeback_flush_all(void) { return TRUE; }
int silencer(int) { return 0; }
int exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
HANDLE floppy_init(int, BOOL) { return NULL; }
HANDLE hd_init(int, BOOL) { return NULL; }
void floppy_final(int) {}
void hd_final(int) {}
int floppy_read(HANDLE,int,ULONG,int,char*) { return 0; }
int floppy_write(HANDLE,int,ULONG,int,char*) { return 0; }
int hd_write(HANDLE,int,ULONG,int,char*) { return 0; }
void floppy_get_geometry(int,int* c,int* h,int* s,int* bytes) { *c=*h=*s=*bytes=0; }
long get_floppy_size(HANDLE,int) { return 0; }
long get_hd_size(HANDLE,int) { return 0; }
int get_floppy_max_size(int) { return 0; }
int is_any_floppy_present(int) { return 0; }
int is_hfs_floppy_present(int) { return 0; }
int is_hfs_hd_present(int) { return 0; }
int try_mount_hd(HANDLE,int) { return 0; }
void get_floppy_volume_file_name(int drive,char* name) { sprintf(name,"%c:\\NO~1_2~3.DSK",'A'+drive); }
void get_hd_volume_file_name(int drive,char* name) { sprintf(name,"%c:\\HP~1_2~3.DSK",'A'+drive); }
void dump_first_cd(char*,ULONG,long) { SetLastError(ERROR_NOT_SUPPORTED); }
int eject_media(char,int) { return 0; }
}
