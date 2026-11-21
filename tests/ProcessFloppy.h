// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Test-only file-backed floppy. No method can open a physical device.
#if defined(HFV_TESTING) && defined(HFV_FLOPPY_TESTING)
#include "FloppyWriter.h"
#include <stdexcept>
class ProcessFloppy final : public FloppyWriteApi {
    HANDLE file_ = INVALID_HANDLE_VALUE;

  public:
    DWORD generation = 7;
    bool locked = false;
    unsigned writes = 0, reads = 0;
    explicit ProcessFloppy(const std::wstring &path) {
        if (path.empty() || path.compare(0, 4, L"\\\\.\\") == 0 || path.compare(0, 4, L"\\\\?\\") == 0)
            throw std::runtime_error("Invalid simulated floppy fixture path");
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER length{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &length) || length.QuadPart != 1474560) {
            if (file_ != INVALID_HANDLE_VALUE)
                CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Simulated floppy requires a 1.44 MB ordinary file");
        }
    }
    ~ProcessFloppy() {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }
    UINT DriveType(int) override { return DRIVE_REMOVABLE; }
    HANDLE Open(const char *, DWORD, DWORD, DWORD) override {
        return reinterpret_cast<HANDLE>(uintptr_t(0x43210000));
    }
    void Close(HANDLE) override {}
    bool Geometry(HANDLE, DISK_GEOMETRY &g) override {
        g = {};
        g.Cylinders.QuadPart = 80;
        g.TracksPerCylinder = 2;
        g.SectorsPerTrack = 18;
        g.BytesPerSector = 512;
        g.MediaType = F3_1Pt44_512;
        return true;
    }
    bool Verify(HANDLE, DWORD &out) override {
        out = generation;
        return true;
    }
    bool Read(HANDLE, ULONGLONG at, DWORD n, void *p, DWORD &got) override {
        ++reads;
        got = 0;
        if (at > 1474560 || n > 1474560 - at) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        LARGE_INTEGER offset;
        offset.QuadPart = at;
        return SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN) && ReadFile(file_, p, n, &got, nullptr) &&
               got == n;
    }
    bool Lock(HANDLE) override {
        locked = true;
        return true;
    }
    bool Dismount(HANDLE) override { return locked; }
    bool Unlock(HANDLE) override {
        locked = false;
        return true;
    }
    bool Write(HANDLE, ULONGLONG at, DWORD n, const void *p, DWORD &got) override {
        ++writes;
        got = 0;
        if (!locked || at > 1474560 || n > 1474560 - at) {
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        LARGE_INTEGER offset;
        offset.QuadPart = at;
        return SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN) &&
               WriteFile(file_, p, n, &got, nullptr) && got == n;
    }
    bool Flush(HANDLE) override { return FlushFileBuffers(file_) != FALSE; }
};
#endif
