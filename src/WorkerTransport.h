// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "CopyWorkerProtocol.h"
#include "ImageHandleTransfer.h"
#include <atomic>

namespace WorkerTransport {
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset(HANDLE h = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = h;
    }
};
struct Process {
    Handle input, output, process, thread, parent;
    DWORD pid = 0;
    ~Process();
    void Launch(const std::vector<ImageHandleTransfer::File>& files, const wchar_t* mode,
                HANDLE extraHandle = nullptr, const wchar_t* executableOverride = nullptr);
};

using CopyWorkerProtocol::Data;
using CopyWorkerProtocol::Message;
void Exact(HANDLE pipe, void* data, DWORD size, bool write);
void Send(HANDLE pipe, Message message, Data data = {});
Message Receive(HANDLE pipe, Data& data);
bool Available(HANDLE pipe);
void SendImageFile(HANDLE pipe, const ImageHandleTransfer::File&);
ImageHandleTransfer::File DecodeImageFile(Data&);
using AttributeSets = std::array<std::optional<HfsPlus::Attributes>, 2>;
void SendAttributes(HANDLE pipe, unsigned slot, const HfsPlus::Attributes&, Storage::Job&);
void ReceiveAttributes(HANDLE pipe, Data& set, AttributeSets&);
#ifdef HFV_TESTING
extern std::atomic<DWORD> lastExitCode;
#endif
}
