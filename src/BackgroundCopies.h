// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
namespace BackgroundCopies {
bool Enabled();bool Active();
bool Start(std::vector<FileOperations::Request>,CWnd* owner,bool singleFile,std::function<void(bool)> completed={},std::function<void(const FileOperations::Completion&)> observed={});
bool StartStorage(std::function<Storage::Result(Storage::Job&)>,CWnd* owner,const char* title,
                  const char* operation,std::function<void(bool)> completed={});
void TransferOwner(HWND closing,HWND replacement);
void CancelAll();void Shutdown();
#ifdef HFV_TESTING
extern bool enabled;unsigned ProgressOwners(HWND);unsigned TaskbarJobs(HWND);unsigned DialogCount();HWND DialogWindow();HWND ModeWindow();Storage::Result LastResult();
#endif
}
