// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>

namespace WindowsMoveUndo {
bool Supported(const std::wstring& folder);
void Recover(const std::string& decision,bool committed);

class Session {
    struct State;
    std::unique_ptr<State> state_;
public:
    Session(const std::wstring& folder,const std::string& decision);
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    static Session* Current();
    std::wstring Slot();
    void Rename(const std::wstring& from,const std::wstring& to);
    void PreserveDirectoryDates(const std::wstring& path);
    void Finish();
};
}
