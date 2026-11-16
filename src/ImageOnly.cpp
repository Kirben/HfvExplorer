// Shared legacy API state and lifecycle for the native CD/floppy backend.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "floppy.h"
#include "filehook.h"
#include "ReadOnlyCdBackend.h"
#include <sys/stat.h>

extern "C" {
flcd_device_type flcds[MAX_DEVICES] = {};
void floppy_module_global_init(void) { readonly_cd_backend_init(); }
void floppy_module_global_final(void) { readonly_cd_backend_final(); }
int exists(const char* path) { struct _stat64 st; return _stat64(path, &st) == 0; }
}
