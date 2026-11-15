// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class CWnd;
// 0: another format; 1: MFS browser handled it; -1: recognized MFS was invalid.
int TryOpenMfsBrowser(const char* path,CWnd* parent);
