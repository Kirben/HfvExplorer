// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void readonly_cd_backend_init(void);
void readonly_cd_backend_final(void);
int readonly_media_path(const char* path);
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
#endif
