// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
#include <atomic>
namespace CopyProcess {
Storage::Result Run(const FileOperations::Request &, Storage::Job &, FileOperations::Completion &,
                    const std::vector<std::string> &, const std::vector<std::string> &);
int Child(const char *, const char *);
#ifdef HFV_TESTING
std::vector<uint8_t> EncodeLocationForTest(const FileOperations::Location &);
FileOperations::Location DecodeLocationForTest(const std::vector<uint8_t> &);
extern std::atomic<DWORD> lastProcess;
extern std::atomic<bool> enabled;
inline constexpr unsigned failWorkerLaunch = 14;
inline constexpr unsigned staleImagePartition = 15;
extern std::atomic<unsigned> fault;
extern std::atomic<HANDLE> ioGate;
extern std::wstring floppyFixture;
extern DWORD floppyGeneration;
#endif
} // namespace CopyProcess
