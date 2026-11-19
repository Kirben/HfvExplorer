// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cwctype>
namespace ImageHandleTransfer {
struct File {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool writable = false;
};
inline const std::vector<File> *&Current() {
    static thread_local const std::vector<File> *files = nullptr;
    return files;
}
class Scope {
    const std::vector<File> *previous_;

  public:
    explicit Scope(const std::vector<File> &files) : previous_(Current()) { Current() = &files; }
    ~Scope() { Current() = previous_; }
};
inline HANDLE Duplicate(HANDLE file, bool inherit = false) {
    HANDLE result = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &result, 0, inherit,
                         DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("Cannot transfer an image handle");
    return result;
}
inline std::wstring Canonical(const std::wstring &path) {
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring out(n, 0);
    if (!n || GetFullPathNameW(path.c_str(), n, &out[0], nullptr) >= n)
        throw std::runtime_error("Cannot identify image path");
    out.resize(wcslen(out.c_str()));
    for (auto &c : out)
        c = wchar_t(towupper(c));
    return out;
}
// Duplication shares the original kernel file object, retaining its sharing
// exclusion continuously. Only the owning worker may use its cursor meanwhile.
inline HANDLE Open(const wchar_t *path, bool write) {
    if (!Current())
        return INVALID_HANDLE_VALUE;
    auto canonical = Canonical(path);
    // NTFS gives named streams the same file ID as the default data stream.
    // Match them by name, so an inherited disk image cannot shadow its streams.
    const auto namedStream = [](const std::wstring& name) {
        const auto separator = name.find_last_of(L"\\/");
        return separator != std::wstring::npos && name.find(L':', separator + 1) != std::wstring::npos;
    };
    const bool isStream = namedStream(canonical);
    BY_HANDLE_FILE_INFORMATION wanted{};
    bool have = false;
    for (const auto &file : *Current()) {
        const auto transferred = Canonical(file.path);
        bool same = canonical == transferred;
        if (!same && !isStream && !namedStream(transferred)) {
            if (!have) {
                HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    have = GetFileInformationByHandle(h, &wanted) != FALSE;
                    CloseHandle(h);
                }
            }
            BY_HANDLE_FILE_INFORMATION info{};
            same = have && GetFileInformationByHandle(file.handle, &info) &&
                   info.dwVolumeSerialNumber == wanted.dwVolumeSerialNumber &&
                   info.nFileIndexHigh == wanted.nFileIndexHigh && info.nFileIndexLow == wanted.nFileIndexLow;
        }
        if (same) {
            if (write && !file.writable)
                throw std::runtime_error("Transferred image is read-only");
            return Duplicate(file.handle);
        }
    }
    return INVALID_HANDLE_VALUE;
}
} // namespace ImageHandleTransfer
