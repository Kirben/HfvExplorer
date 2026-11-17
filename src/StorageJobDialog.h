// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
void InitializeStorageUi();
bool StorageOwnerShouldActivate(bool originallyEnabled,HWND owner,HWND dialog,HWND foreground);
// Notification refreshes must not activate/disable an unrelated browser or
// show progress popups. Reads stay on the worker; only paint/timer/notification
// messages are dispatched while their snapshots are obtained.
class StorageQuietRefresh {
public:
 StorageQuietRefresh();~StorageQuietRefresh();
 StorageQuietRefresh(const StorageQuietRefresh&)=delete;
 StorageQuietRefresh& operator=(const StorageQuietRefresh&)=delete;
};
class StorageAction {
 bool active_;
public:
 StorageAction();
 ~StorageAction();
 StorageAction(const StorageAction&)=delete;
 StorageAction& operator=(const StorageAction&)=delete;
};
Storage::Result ShowStorageJob(std::function<Storage::Result(Storage::Job&)> work,const char* title="Working",bool hidden=false,bool singleFile=false,bool cancellable=false,CWnd* owner=nullptr);
Storage::Result ShowStorageWaitJob(std::function<Storage::Result(Storage::Job&)> work,bool hidden=false,CWnd* owner=nullptr);
#ifdef HFV_TESTING
struct StorageUiTestState {bool disabledDuring=false,enabledAfter=false,dialogClosed=false,ownerRestoredBeforeClose=false;HANDLE releaseAfterTwoTimers=nullptr;unsigned timerTicks=0,dialogCreations=0,copyBarCount=0;bool hiddenNative=false,simpleStorageLayout=false,cancelOnTimer=false;DWORD explanationDelayMs=5000;unsigned explanationShows=0,spinnerTicks=0;std::string storageMessage;Storage::ConflictChoice answer=Storage::ConflictChoice::None;};
extern StorageUiTestState storageUiTest;
#endif
Storage::Result ShowSubmittedStorageJob(const std::shared_ptr<Storage::Job>&,const char* title="Working",bool hidden=false,bool singleFile=false,bool cancellable=true,CWnd* owner=nullptr);
void ShutdownStorageUi();
