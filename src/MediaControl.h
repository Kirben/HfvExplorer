// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include "CommandStorage.h"
#include <winioctl.h>
struct MediaControlTarget {int drive=-1;UINT type=DRIVE_UNKNOWN;};
bool CanControlMedia(MediaControlTarget target);
MediaControlTarget MacMediaControlTarget(const char* path);
class MediaControlApi {
public:
 virtual ~MediaControlApi(){}
 virtual UINT DriveType(int drive)=0;
 virtual HANDLE Open(int drive,DWORD access)=0;
 virtual bool Control(HANDLE handle,DWORD code)=0;
 virtual bool Close(HANDLE handle)=0;
 virtual void Pause(){Sleep(50);}
};
std::function<Storage::Result(Storage::Job&)> MakeMediaControlJob(MediaControlTarget target,bool reload,MediaControlApi* api=nullptr,std::function<void()> release={},CommandStorage::Reservation held={});
