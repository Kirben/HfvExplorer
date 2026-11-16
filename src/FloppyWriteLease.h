// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <set>
#include <mutex>
#include <cctype>
#include <cstdio>

// Cooperative cross-process guard, acquired before probing or opening a writer.
// The native FSCTL_LOCK_VOLUME is still required before any write/dismount.
class FloppyWriteLease {
    HANDLE mutex_ = nullptr;
    std::string key_;
    static std::mutex& RegistryMutex() { static std::mutex value; return value; }
    static std::set<std::string>& Registry() { static std::set<std::string> value; return value; }
public:
    FloppyWriteLease() = default;
    FloppyWriteLease(const FloppyWriteLease&) = delete;
    FloppyWriteLease& operator=(const FloppyWriteLease&) = delete;
    ~FloppyWriteLease() { Release(); }
    void Release() {
        if (!mutex_) return;
        ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr;
        std::lock_guard<std::mutex> lock(RegistryMutex()); Registry().erase(key_); key_.clear();
    }
    bool Acquire(int drive) {
        if (mutex_ || drive < 0 || drive > 25) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        char letter[] = "A:", device[32768]; letter[0] += char(drive);
        // Resolve drive aliases to the same device. QueryDosDevice performs no media I/O.
        std::string key = QueryDosDeviceA(letter, device, sizeof(device)) ? device : letter;
        for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
        unsigned long long hash = 14695981039346656037ULL;
        for (unsigned char c : key) { hash ^= c; hash *= 1099511628211ULL; }
        char name[100]; sprintf(name, "Global\\HFVExplorer.FloppyWriter.v1.%016llx", hash);
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            // Windows mutexes are recursive; a second writer in this process is not allowed.
            if (!Registry().insert(key).second) { SetLastError(ERROR_SHARING_VIOLATION); return false; }
        }
        HANDLE handle = CreateMutexA(nullptr, FALSE, name);
        DWORD error = GetLastError();
        DWORD wait = handle ? WaitForSingleObject(handle, 0) : WAIT_FAILED;
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            mutex_ = handle; key_ = key; return true;
        }
        if (handle) { error = wait == WAIT_TIMEOUT ? ERROR_SHARING_VIOLATION : GetLastError(); CloseHandle(handle); }
        { std::lock_guard<std::mutex> lock(RegistryMutex()); Registry().erase(key); }
        SetLastError(error); return false;
    }
};
