// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include <optional>
// Run a filesystem mutation under an exclusive lock. The callback must close
// its filesystem handles before returning. Return only after device readback.
void RunFloppyWrite(int drive,std::optional<DWORD> generation,const std::function<void()>& mutate);
