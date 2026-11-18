// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "WorkerTransport.h"

namespace WorkerTransport {
using namespace CopyWorkerProtocol;
#ifdef HFV_TESTING
std::atomic<DWORD> lastExitCode{0};
#endif

void Exact(HANDLE pipe, void *data, DWORD n, bool write) {
    auto p = static_cast<uint8_t *>(data);
    while (n) {
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, p, n, &done, nullptr) : ReadFile(pipe, p, n, &done, nullptr);
        Check(ok && done, "Copy-worker connection closed unexpectedly; completed changes may remain. Inspect "
                          "the destination before retrying");
        p += done;
        n -= done;
    }
}
void Send(HANDLE pipe, Message message, Data data) {
    Check(data.bytes.size() <= FrameLimit(message), "Copy-worker packet exceeds the protocol frame size");
    DWORD header[] = {DWORD(protocol), DWORD(message), DWORD(data.bytes.size())};
    Exact(pipe, header, sizeof(header), true);
    if (!data.bytes.empty())
        Exact(pipe, data.bytes.data(), DWORD(data.bytes.size()), true);
}
Message Receive(HANDLE pipe, Data &data) {
    DWORD header[3];
    Exact(pipe, header, sizeof(header), false);
    auto message = ValidateFrame(header);
    data = Data();
    data.bytes.resize(header[2]);
    if (header[2])
        Exact(pipe, data.bytes.data(), header[2], false);
    return message;
}
bool Available(HANDLE pipe) {
    DWORD n = 0;
    Check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &n, nullptr) != FALSE,
          "Copy-worker control connection closed");
    return n != 0;
}
void SendImageFile(HANDLE pipe,const ImageHandleTransfer::File& file) {
    Data data;
    data.wide(file.path); data.put(uintptr_t(file.handle)); data.put(file.writable);
    Send(pipe, File, std::move(data));
}
ImageHandleTransfer::File DecodeImageFile(Data& data) {
    auto path=data.wide();
    auto handle=HANDLE(uintptr_t(data.get()));
    const auto writable=data.flag();
    data.end();
    return {std::move(path),handle,writable};
}

constexpr size_t attributeChunk = 65536;

void SendAttributes(HANDLE pipe, unsigned slot, const HfsPlus::Attributes& attributes,
                    Storage::Job& job) {
    Data set;
    set.put(slot); set.put(attributes.size());
    Send(pipe, AttributeSet, std::move(set));
    std::vector<char> buffer(attributeChunk);
    for (const auto& attribute : attributes) {
        Data header;
        header.u16(attribute.first); header.put(attribute.second.size());
        Send(pipe, AttributeHeader, std::move(header));
        for (uint64_t at = 0; at < attribute.second.size();) {
            if (job.cancelled()) throw Storage::Cancelled();
            const auto count = size_t((std::min)(uint64_t(buffer.size()), attribute.second.size() - at));
            attribute.second.Read(at, buffer.data(), count);
            Data chunk;
            chunk.bytes.assign(buffer.data(), buffer.data() + count);
            Send(pipe, AttributeChunk, std::move(chunk));
            at += count;
        }
    }
}

void ReceiveAttributes(HANDLE pipe, Data& set, AttributeSets& sets) {
    auto slot = set.get(), count = set.get();
    set.end();
    Check(slot < sets.size() && !sets[size_t(slot)], "Invalid worker attribute set");
    HfsPlus::Attributes attributes;
    for (uint64_t i = 0; i < count; ++i) {
        Data header;
        Check(Receive(pipe, header) == AttributeHeader, "Expected worker attribute header");
        auto name = header.u16();
        auto length = header.get();
        header.end();
        Check(!name.empty() && !attributes.count(name), "Invalid or duplicate worker attribute name");
        auto value = HfsPlus::AttributeData::Snapshot(length, [&](uint64_t, void* bytes, size_t size) {
            auto output = static_cast<uint8_t*>(bytes);
            while (size) {
                Data chunk;
                Check(Receive(pipe, chunk) == AttributeChunk &&
                      chunk.bytes.size() == (std::min)(size, attributeChunk),
                      "Invalid worker attribute chunk");
                memcpy(output, chunk.bytes.data(), chunk.bytes.size());
                output += chunk.bytes.size();
                size -= chunk.bytes.size();
            }
        });
        attributes.emplace(std::move(name), std::move(value));
    }
    sets[size_t(slot)] = std::move(attributes);
}

Process::~Process() {
    input.reset();
    output.reset();
    if (process.value) {
        WaitForSingleObject(process.value, INFINITE);
#ifdef HFV_TESTING
        DWORD code = 0;
        if (GetExitCodeProcess(process.value, &code)) lastExitCode = code;
#endif
    }
}

void Process::Launch(const std::vector<ImageHandleTransfer::File>& files, const wchar_t* mode,
                 HANDLE extraHandle, const wchar_t* executableOverride) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    Check(CreatePipe(&inRead, &inWrite, &security, 0) != FALSE, "Cannot create worker control pipe");
    Handle childInput(inRead);
    input.reset(inWrite);
    Check(SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) != FALSE,
          "Cannot protect control pipe handle");
    Check(CreatePipe(&outRead, &outWrite, &security, 0) != FALSE, "Cannot create worker progress pipe");
    output.reset(outRead);
    Handle childOutput(outWrite);
    Check(SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0) != FALSE,
          "Cannot protect progress pipe handle");
    parent.reset(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
    Check(parent.value != nullptr, "Cannot monitor browser lifetime");
    std::vector<HANDLE> inherited{childInput.value, childOutput.value, parent.value};
    for (auto &f : files)
        inherited.push_back(f.handle);
    if (extraHandle) inherited.push_back(extraHandle);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributes(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    Check(InitializeProcThreadAttributeList(list, 1, 0, &size) != FALSE,
          "Cannot initialize worker handle whitelist");
    struct DeleteList {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~DeleteList() { DeleteProcThreadAttributeList(list); }
    } cleanup{list};
    Check(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                    inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE,
          "Cannot restrict inherited worker handles");
    wchar_t executable[32768];
    DWORD n = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    Check(n && n < ARRAYSIZE(executable), "Cannot locate copy worker executable");
#ifdef HFV_TESTING
    // Lets fixture-only tests exercise the actual shipping worker binary.
    wchar_t overridePath[32768];
    DWORD overrideSize = GetEnvironmentVariableW(L"HFV_COPY_WORKER_TEST_EXECUTABLE", overridePath,
                                                 ARRAYSIZE(overridePath));
    if (overrideSize) {
        Check(overrideSize < ARRAYSIZE(overridePath), "Invalid test worker executable path");
        wcscpy_s(executable, overridePath);
    }
#endif
    if (executableOverride) wcscpy_s(executable, executableOverride);
    auto command = L"\"" + std::wstring(executable) + L"\" " + mode + L" " +
                   std::to_wstring(uintptr_t(childInput.value)) + L" " +
                   std::to_wstring(uintptr_t(childOutput.value));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    Check(CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                         &startup.StartupInfo, &info) != FALSE,
          "Cannot start copy worker; no copy was performed");
    process.reset(info.hProcess);
    thread.reset(info.hThread);
    pid = info.dwProcessId;
}
}
