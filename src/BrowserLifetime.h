// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class CHFVExplorerDoc;
namespace BrowserLifetime {
// Viewless documents retain physical-reader references until copy I/O is idle.
// Every function runs on the UI thread; workers never own document pointers.
void Retire(CHFVExplorerDoc*);
void Collect();
#ifdef HFV_TESTING
unsigned RetiredCount();
#endif
}
