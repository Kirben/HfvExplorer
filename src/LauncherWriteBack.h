// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"

class BrowserSession;

class LauncherTemporaryDocument {
public:
    LauncherTemporaryDocument() = default;
    ~LauncherTemporaryDocument();
    LauncherTemporaryDocument(const LauncherTemporaryDocument&) = delete;
    LauncherTemporaryDocument& operator=(const LauncherTemporaryDocument&) = delete;
    Storage::Result Open(const wchar_t* path,bool cleanup);
    Storage::Result ReadVersion(uint64_t& result,Storage::Job& job) const;
    bool Remove() const;
    HANDLE Handle() const { return file_; }

private:
    HANDLE file_=INVALID_HANDLE_VALUE;
};

class LauncherHandoff {
public:
    LauncherHandoff()=default;
    LauncherHandoff(const LauncherHandoff&)=delete;
    LauncherHandoff& operator=(const LauncherHandoff&)=delete;
    ~LauncherHandoff(){ Close(); }
    Storage::Result Protect(const wchar_t* path,const MacBrowser::ObjectIdentity& expected,bool readOnly);
    void Close();

private:
    HANDLE file_=INVALID_HANDLE_VALUE;
};

class LauncherWriteBack {
public:
    LauncherWriteBack(FileOperations::Location original,std::string version,CString temporary);
    ~LauncherWriteBack();
    LauncherWriteBack(const LauncherWriteBack&) = delete;
    LauncherWriteBack& operator=(const LauncherWriteBack&) = delete;
    bool CaptureBaseline(CWnd* owner);
    void Track(HANDLE process) { process_=process; started_=GetTickCount(); }
    bool Ready() const;
    void Complete(BrowserSession&);

private:
    FileOperations::Location original_;
    std::string version_;
    CString temporary_;
    HANDLE process_=nullptr;
    DWORD started_=0;
    uint64_t baseline_=0;
};
