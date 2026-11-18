// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void readonly_cd_backend_init(void);
void readonly_cd_backend_final(void);
int readonly_media_path(const char* path);
int physical_floppy_path(const char* path);
int physical_floppy_writable(const char* path);
// Release the native read handle; outstanding readers reopen on next use.
void physical_floppy_suspend(int drive);
int physical_cd_generation(int drive,unsigned long* generation);
int physical_floppy_generation(int drive,unsigned long* generation);
// Invalidate all descriptors and read caches for a physical media operation.
void native_media_suspend(int drive);
#ifdef __cplusplus
}
#endif
#ifdef HFV_CD_TESTING
// Test fixtures never enable a write handle and are absent in shipping builds.
bool readonly_cd_attach_fixture(int drive, const char* path);
#endif
#if defined(HFV_FLOPPY_TESTING) && defined(__cplusplus)
class FloppyReadApi;
bool readonly_floppy_attach_api(int drive,FloppyReadApi* api);
FloppyReadApi* physical_floppy_test_api(int drive);
#endif
