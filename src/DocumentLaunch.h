// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"

class BrowserSession;
namespace DocumentLaunch {
enum class Target { Default, ExecutorDos, ExecutorWin32, VMac, ShapeShifter, Fusion, Windows };

struct Request {
 FileOperations::Location selection;
 MacBrowser::Entry entry;
 std::shared_ptr<BrowserDirectory::OwnedIcons> icons;
 HICON small=nullptr,large=nullptr;
 int classicSlot=-1;
 Target target=Target::Default;
 bool shortcut=false;
};

Target FromMenuCommand(int command);
bool Launch(BrowserSession& session,Request request);
bool OpenWith(BrowserSession& session,const FileOperations::Location& source,
              const FileOperations::Location& application,bool mac);
bool SaveFileIcon(HICON small,HICON large,const char* path,bool document);
CString ExecutorCommand(const CString& configured,const FileOperations::Location& source,CString application);
CString DiskArguments(const FileOperations::Location& source,const CString& operation);
std::wstring WindowsArgument(const FileOperations::Location& source);
}
