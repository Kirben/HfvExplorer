// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
int TryOpenHfsPlusBrowser(const char *path, CWnd *parent);
void FormatHfsPlusImage(CWnd *parent);

// Drive roots only: 0 = ordinary browsing, 1 = CD browser closed, -1 = detected-volume error.
int TryOpenHfsPlusCdBrowser(const char *path, CWnd *parent);
