// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"

namespace VolumeSession {
bool CopiesWithinMountedImage(const FileOperations::OrdinaryCopy&);
bool CopyWithinMountedImage(MacBrowser::Volume&, const std::vector<uint32_t>&, uint32_t, Storage::Job&);
enum class ImageFormat { HfsPlus, Classic, Mfs };
std::shared_ptr<MacBrowser::Volume> MountPersistentImage(const std::wstring &path, const std::string &key,
                                                       const std::wstring& journal, ImageFormat format = ImageFormat::HfsPlus);
int Child(const char *input, const char *output);
#ifdef HFV_TESTING
void PauseVolumeReadForTest(HANDLE entered, HANDLE release);
void PauseVolumeWriteForTest(HANDLE entered, HANDLE release);
DWORD VolumeSessionPidForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionOnRenameForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionDuringPutForTest(const std::shared_ptr<MacBrowser::Volume>&);
#endif
}
