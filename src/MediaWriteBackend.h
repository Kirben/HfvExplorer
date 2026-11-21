// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include "HfsPlus.h"

namespace Storage {
struct MediaIoError : HfsPlus::Error {
    DWORD code;
    MediaIoError(DWORD error,const std::string& message)
        : HfsPlus::Error(message),code(error?error:ERROR_IO_DEVICE) {}
};

// Mounted physical volumes own the verified write scope. Its return marks
// completion of the backend's media checks, flushing and readback.
class MediaWriteBackend {
public:
    virtual ~MediaWriteBackend() = default;
    virtual bool SupportsCoordinatedMove() const { return false; }
    virtual void WithVerifiedWrite(const std::function<void()>& work) = 0;
};
}
