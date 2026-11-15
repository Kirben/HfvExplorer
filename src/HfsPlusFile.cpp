// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include "HfsPlusDisk.h"
namespace HfsPlus {
namespace {
class Image final : public Source {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    bool writable_ = false;
    void Io(uint64_t offset, void *bytes, size_t n, bool write) {
        Disk::Check(Disk::Range(offset, n, size_), "Image I/O range overflow");
        LARGE_INTEGER at;
        at.QuadPart = offset;
        if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN))
            throw Error("Cannot seek HFS+ image");
        auto p = static_cast<uint8_t *>(bytes);
        while (n) {
            DWORD requested = DWORD((std::min)(n, size_t(1024 * 1024))), done = 0;
            BOOL ok = write ? WriteFile(file_, p, requested, &done, nullptr)
                            : ReadFile(file_, p, requested, &done, nullptr);
            if (!ok || done != requested)
                throw Error(write ? "Cannot write HFS+ image" : "Cannot read HFS+ image");
            p += done;
            n -= done;
        }
    }

  public:
    Image(const wchar_t *path, bool write, uint64_t createSize) {
        Disk::Check(path && *path && wcsncmp(path, L"\\\\.\\", 4) != 0 &&
                        wcsncmp(path, L"\\\\?\\GLOBALROOT", 14) != 0,
                    "Physical device paths are not HFS+ images");
        file_ = CreateFileW(path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ, nullptr,
                            createSize ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Disk::Check(file_ != INVALID_HANDLE_VALUE, "Cannot open HFS+ image (it may be in use)");
        try {
            Disk::Check(GetFileType(file_) == FILE_TYPE_DISK, "HFS+ requires a disk image file");
            if (createSize) {
                DWORD returned;
                DeviceIoControl(file_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
                LARGE_INTEGER at;
                at.QuadPart = createSize;
                Disk::Check(SetFilePointerEx(file_, at, nullptr, FILE_BEGIN) && SetEndOfFile(file_),
                            "Cannot size HFS+ image");
            }
            BY_HANDLE_FILE_INFORMATION info = {};
            Disk::Check(GetFileInformationByHandle(file_, &info) && info.nNumberOfLinks &&
                            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                        "HFS+ requires an ordinary image file");
            LARGE_INTEGER length;
            Disk::Check(GetFileSizeEx(file_, &length) && length.QuadPart >= 0, "Invalid HFS+ image length");
            size_ = length.QuadPart;
            writable_ = write;
        } catch (...) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    ~Image() {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }
    uint64_t Size() const override { return size_; }
    bool Writable() const override { return writable_; }
    void Read(uint64_t o, void *p, size_t n) override { Io(o, p, n, false); }
    void Write(uint64_t o, const void *p, size_t n) override {
        Disk::Check(writable_, "HFS+ image is read-only");
        Io(o, const_cast<void *>(p), n, true);
    }
    void Flush() override {
        if (writable_ && !FlushFileBuffers(file_))
            throw Error("Cannot flush HFS+ image");
    }
};
} // namespace
std::unique_ptr<Source> OpenImage(const wchar_t *path, bool write) {
    return std::unique_ptr<Source>(new Image(path, write, 0));
}
std::unique_ptr<Source> CreateImage(const wchar_t *path, uint64_t size) {
    Disk::Check(size >= 8 * 1024 * 1024 && size % 4096 == 0 && size <= INT64_MAX,
                "HFS+ image size must be at least 8 MiB and a multiple of 4096");
    return std::unique_ptr<Source>(new Image(path, true, size));
}
} // namespace HfsPlus
