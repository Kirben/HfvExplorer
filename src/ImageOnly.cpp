// Legacy hard-disk stubs; CDs/floppies use the read-only Windows backend.
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
int exists(const char* path) { struct _stat64 st; return _stat64(path, &st) == 0; }
HANDLE hd_init(int, BOOL) { return NULL; }
void hd_final(int) {}
int hd_write(HANDLE,int,ULONG,int,char*) { return 0; }
long get_hd_size(HANDLE,int) { return 0; }
int is_hfs_hd_present(int) { return 0; }
int try_mount_hd(HANDLE,int) { return 0; }
void get_hd_volume_file_name(int drive,char* name) { sprintf(name,"%c:\\HP~1_2~3.DSK",'A'+drive); }
void dump_first_cd(char*,ULONG,long) { SetLastError(ERROR_NOT_SUPPORTED); }
int eject_media(char,int) { return 0; }
}
