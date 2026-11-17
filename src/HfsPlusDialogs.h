// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
class BrowserSession;
void FormatHfsPlusImage(CWnd *parent,BrowserSession& session);
void HfvUnicodeNameEdit(HWND,int);
bool EditHfsPlusName(CWnd *parent, std::wstring &name, bool caseSensitive);
