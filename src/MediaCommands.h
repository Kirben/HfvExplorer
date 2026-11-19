// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "CommandStorage.h"

class CWnd;

namespace MediaCommands {
using Work=std::function<Storage::Result(Storage::Job&)>;
using Prepare=std::function<Work(CommandStorage::Reservation)>;

// UI-thread entry point. The device work runs under the reservation on a worker.
Storage::Result Start(CWnd& owner,int drive,const char* title,const char* operation,const Prepare&);
}
