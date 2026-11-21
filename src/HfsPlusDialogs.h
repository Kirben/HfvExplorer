// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include "DmgEncryption.h"
#include "StorageWorker.h"
#include <string>
class BrowserSession;
namespace MacBrowser { class Volume; }
enum class ApfsImageMode {None,CaseInsensitive,CaseSensitive};
struct MacImageFormatRequest {
    std::wstring path,externalJournal;
    std::u16string name;
    uint64_t size=0;
    HfsPlus::Volume::FormatKind kind=HfsPlus::Volume::FormatKind::HfsPlus;
    bool journaled=true,mfs=false,dc42=false,expanded=false;
    ApfsImageMode apfs=ApfsImageMode::None;
};
std::function<Storage::Result(Storage::Job&)> MakeMacImageFormatJob(MacImageFormatRequest);
bool StartMacImageFormat(BrowserSession&,MacImageFormatRequest);
void FormatHfsPlusImage(CWnd *parent,BrowserSession& session);
Dmg::Credentials PromptDmgCredentials(CWnd*,Dmg::UnlockMethod,bool retry,const std::wstring* apfsHint=nullptr);
void ConvertDmgImage(BrowserSession&);
void ManageApfsSnapshots(CWnd*);
void HfvUnicodeNameEdit(HWND,int);
bool EditWindowsName(CWnd *parent, std::wstring &name);
bool EditMacVolumeName(CWnd *parent, std::wstring &name, const MacBrowser::Volume& volume);

bool SelectHfsPlusExternalJournal(CWnd*,std::wstring&,const std::string&);
