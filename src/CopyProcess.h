// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
#include <atomic>
namespace CopyProcess {
bool UsesWorker(const FileOperations::Request &);
Storage::Result Run(const FileOperations::Request &, Storage::Job &, FileOperations::Completion &,
                    const std::vector<std::string> &, const std::vector<std::string> &);
int Child(const char *, const char *);
#ifdef HFV_TESTING
extern std::atomic<DWORD> lastProcess;
extern std::atomic<bool> enabled;
extern std::atomic<unsigned> fault;
extern std::wstring floppyFixture;
extern DWORD floppyGeneration;
#endif
} // namespace CopyProcess
