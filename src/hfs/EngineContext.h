// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(_MSC_VER)
#define HFV_THREAD_LOCAL __declspec(thread)
#else
#define HFV_THREAD_LOCAL __thread
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct hfs_engine_context {
 struct _hfsvol_ *mounts, *current;
 char *error;
 struct ClassicHfsTransaction *mutation;
 char failure_after_close[512];
};
struct hfs_engine_context *hfs_context_get(void);
struct hfs_engine_context *hfs_context_use(struct hfs_engine_context *);
#ifdef __cplusplus
}
#endif
