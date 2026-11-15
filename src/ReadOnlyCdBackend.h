// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
extern "C" {
void readonly_cd_backend_init(void);
void readonly_cd_backend_final(void);
}
#ifdef HFV_CD_TESTING
// Test fixtures never enable a write handle and are absent in shipping builds.
bool readonly_cd_attach_fixture(int drive, const char* path);
#endif
