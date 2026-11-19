// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include "StorageWorker.h"
#include <string>
class BrowserSession;
struct MacImageFormatRequest {
    std::wstring path,externalJournal;
    std::u16string name;
    uint64_t size=0;
    HfsPlus::Volume::FormatKind kind=HfsPlus::Volume::FormatKind::HfsPlus;
    bool journaled=true,mfs=false,dc42=false,expanded=false;
};
std::function<Storage::Result(Storage::Job&)> MakeMacImageFormatJob(MacImageFormatRequest);
bool StartMacImageFormat(BrowserSession&,MacImageFormatRequest);
void FormatHfsPlusImage(CWnd *parent,BrowserSession& session);
namespace Dmg {class Password;}
std::shared_ptr<const Dmg::Password> PromptDmgPassword(CWnd* parent,bool retry);
void ConvertDmgImage(BrowserSession&);
void HfvUnicodeNameEdit(HWND,int);
bool EditWindowsName(CWnd *parent, std::wstring &name);
bool EditHfsPlusName(CWnd *parent, std::wstring &name, bool caseSensitive);

bool SelectHfsPlusExternalJournal(CWnd*,std::wstring&,const std::string&);
