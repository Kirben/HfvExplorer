// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
#include <atomic>
namespace CopyProcess {
bool UsesWorker(const FileOperations::Request &);
bool ConvertsWithMountedClassic(const FileOperations::LegacyConversion&);
bool CopiesWithinMountedImage(const FileOperations::OrdinaryCopy&);
bool CopyWithinMountedImage(MacBrowser::Volume&, const std::vector<uint32_t>&, uint32_t, Storage::Job&);
Storage::Result Run(const FileOperations::Request &, Storage::Job &, FileOperations::Completion &,
                    const std::vector<std::string> &, const std::vector<std::string> &);
int Child(const char *, const char *);
enum class ImageFormat { HfsPlus, Classic, Mfs };
std::shared_ptr<MacBrowser::Volume> MountPersistentImage(const std::wstring &path, const std::string &key,
                                                       const std::wstring& journal, ImageFormat format = ImageFormat::HfsPlus);
int VolumeSessionChild(const char *input, const char *output);
#ifdef HFV_TESTING
std::vector<uint8_t> EncodeLocationForTest(const FileOperations::Location &);
FileOperations::Location DecodeLocationForTest(const std::vector<uint8_t> &);
extern std::atomic<DWORD> lastProcess, lastExitCode;
DWORD VolumeSessionPidForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionOnRenameForTest(const std::shared_ptr<MacBrowser::Volume>&);
void CrashVolumeSessionDuringPutForTest(const std::shared_ptr<MacBrowser::Volume>&);
extern std::atomic<bool> enabled;
inline constexpr unsigned failWorkerLaunch = 14;
extern std::atomic<unsigned> fault;
extern std::atomic<HANDLE> ioGate;
extern std::wstring floppyFixture;
extern DWORD floppyGeneration;
#endif
} // namespace CopyProcess
