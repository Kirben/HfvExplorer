// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FindFiles.h"
class CWnd;
class BrowserSession;
using FindCompletion=std::function<void(BrowserSession&,std::shared_ptr<const FindResults>,const Storage::Result&)>;
bool StartFindSearch(BrowserSession&,FindRequest,FindCompletion);
void ShowFindFilesDialog(BrowserSession&,FindScope);
