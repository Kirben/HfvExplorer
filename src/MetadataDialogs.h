// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
namespace MetadataDialogs {
bool Show(FileOperations::Location,CWnd*);
void Shutdown();
#ifdef HFV_TESTING
HWND Window();
std::string LastError();
bool ImportValue(HWND,FileOperations::Location,const std::u16string&);
unsigned CloseDuringDispatch(HWND);
#endif
}
