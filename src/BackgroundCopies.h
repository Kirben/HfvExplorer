// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
namespace BackgroundCopies {
bool Enabled();bool Active();
bool Start(std::vector<FileOperations::Request>,CWnd* owner,bool singleFile,std::function<void(bool)> completed={});
void CancelAll();void Shutdown();
#ifdef HFV_TESTING
extern bool enabled;unsigned DialogCount();HWND DialogWindow();HWND ModeWindow();Storage::Result LastResult();
#endif
}
