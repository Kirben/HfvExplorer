// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
#include "DiskPartitions.h"

namespace VolumeSession {
bool SharesMountedImage(const FileOperations::Location& source,const FileOperations::Location& destination);
uint32_t PutFromMountedSource(MacBrowser::Volume&,uint32_t parent,const MacBrowser::Entry&,bool replace,
                         std::function<void(unsigned,uint64_t,size_t)> progress);
enum class ImageFormat { HfsPlus, Classic, Mfs, NdifHfsPlus, NdifClassic, NdifMfs };
std::shared_ptr<MacBrowser::Volume> MountPersistentImage(const std::wstring &path, const std::string &key,
                                                       const std::wstring& journal, ImageFormat format,
                                                       std::optional<DiskPartitions::Selection> selection = {});
int Child(const char *input, const char *output);
#ifdef HFV_TESTING
void PauseVolumeReadForTest(HANDLE entered, HANDLE release);
void PauseVolumeWriteForTest(HANDLE entered, HANDLE release);
DWORD VolumeSessionPidForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionOnRenameForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionDuringPutForTest(const std::shared_ptr<MacBrowser::Volume>&);
#endif
}
