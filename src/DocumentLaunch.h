// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"

class BrowserSession;
namespace DocumentLaunch {

struct Request {
 FileOperations::Location selection;
 MacBrowser::Entry entry;
};

void ShowBasiliskII(CWnd* owner);
void ShowMiniVMac(CWnd* owner);
#ifdef HFV_TESTING
bool ProbeEmulatorLaunchers(CWnd* owner, const char* directory);
#endif
bool Launch(BrowserSession& session,Request request);
bool OpenWith(BrowserSession& session,const FileOperations::Location& source,
              const FileOperations::Location& application);
bool SaveFileIcon(HICON small,HICON large,const char* path,bool document);
std::wstring WindowsArgument(const FileOperations::Location& source);
}
