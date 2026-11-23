// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
struct LegacyCopyOptions {char output='a',input='a';bool appledouble=true,verifyFloppy=false,targetGenerationKnown=false;DWORD targetGeneration=0;};
LegacyCopyOptions CaptureLegacyCopyOptions(char output);
class BrowserSession;
namespace FileOperations { class Location; }
namespace MacBrowser { struct ObjectIdentity; }
bool StageWindowsLaunchDocument(BrowserSession&,const FileOperations::Location&,CString& output,MacBrowser::ObjectIdentity& identity);
