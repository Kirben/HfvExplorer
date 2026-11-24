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
    Storage::Result CheckWriteBackStreams() const;
    bool Remove(const std::function<bool()>& finish={}) const;
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

CString LauncherPendingRoot();

class LauncherWriteBack {
public:
    LauncherWriteBack(FileOperations::Location original,std::string version,CString temporary);
    LauncherWriteBack(const LauncherWriteBack&) = delete;
    LauncherWriteBack& operator=(const LauncherWriteBack&) = delete;
    ~LauncherWriteBack();
    bool CaptureBaseline(CWnd* owner);
    bool SavePending();
    Storage::Result CheckCommitStatus(bool& committed) const;
    Storage::Result ApplyPending(const LauncherTemporaryDocument&,Storage::Job&);
    bool RetireCommitted();
    bool FinishUnchanged(const LauncherTemporaryDocument&);
    static std::vector<std::unique_ptr<LauncherWriteBack>> LoadPending(const CString& root, std::vector<CString>* preserved=nullptr);
    const CString& TemporaryPath() const { return temporary_; }
    bool Complete(BrowserSession&);

private:
    FileOperations::Location original_;
    std::string version_;
    CString temporary_;
    bool Forget();
    HANDLE pending_=INVALID_HANDLE_VALUE;
    DWORD decisionOffset_=0;
    std::string decisionId_;
    uint64_t baseline_=0;
};
