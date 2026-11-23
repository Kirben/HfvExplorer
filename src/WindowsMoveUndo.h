// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <windows.h>
#include <memory>
#include <map>
#include <string>
#include "WindowsEntryIdentity.h"

namespace WindowsMoveUndo {
#ifdef HFV_TESTING
extern thread_local bool forceUnsupportedForTest;
#endif
bool Supported(const std::wstring& folder);
void Recover(const std::string& decision,bool committed);
void RecoverExports(const std::wstring& folder);
void RunExport(const std::wstring& folder,const std::function<void()>& work);

class Session {
    struct State;
    std::unique_ptr<State> state_;
    void RenameChecked(const std::wstring& from,const std::wstring& to,
                       const MacBrowser::ObjectIdentity* expected,
                       const std::map<std::wstring,MacBrowser::ObjectIdentity>* tree,bool relabel=false);
public:
    Session(const std::wstring& folder,const std::string& decision,bool exportOnly=false);
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    static Session* Current();
    std::wstring Slot();
    std::wstring StageTemporary(const std::wstring& path,HANDLE created=INVALID_HANDLE_VALUE);
    void Rename(const std::wstring& from,const std::wstring& to,
                const MacBrowser::ObjectIdentity* expected=nullptr);
    void Relabel(const std::wstring& from,const std::wstring& to,
                 const MacBrowser::ObjectIdentity& expected);
    void StageCut(const MacBrowser::WindowsCutPlan&);
    void PreserveDirectoryDates(const std::wstring& path);
    void Finish();
};
}
