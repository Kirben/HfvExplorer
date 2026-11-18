// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

struct FloppyWriteFailure : std::runtime_error {
 DWORD error;
 FloppyWriteFailure(DWORD code,const std::string& message)
     : std::runtime_error(message),error(code?code:ERROR_WRITE_FAULT){}
};

// Run a filesystem mutation under an exclusive lock. The callback must close
// its filesystem handles before returning. Return only after device readback.
bool RunFloppyWrite(int drive,std::optional<DWORD> generation,const std::function<void()>& mutate);
