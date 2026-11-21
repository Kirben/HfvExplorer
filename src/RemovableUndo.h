// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace RemovableUndo {
struct Range {
    uint64_t offset, length;
    DWORD sector;
};
using Read = std::function<void(uint64_t,DWORD,void*)>;
using Restore = std::function<void(uint64_t,DWORD,const void*)>;

bool Enabled();
std::wstring FloppyKey(int drive);
bool Pending(const std::wstring& device);
Range PendingRange(const std::wstring& device);
void Recover(const std::wstring& device,const Range&,const Read&,const Restore&);

class Log {
    struct State;
    std::unique_ptr<State> state_;
public:
    Log(const std::wstring& device,const Range&,const Read&);
    ~Log();
    void BeforeWrite(uint64_t offset,DWORD count,const void* before,const void* after);
    void Commit();
};

#ifdef HFV_TESTING
void TestCheckpoint(unsigned phase);
#endif
}
