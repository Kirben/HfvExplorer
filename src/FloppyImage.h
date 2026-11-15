// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
class FloppyReadApi;
class CWnd;
std::function<Storage::Result(Storage::Job&)> MakeFloppyImageJob(int drive,std::string output,FloppyReadApi* api=nullptr);
