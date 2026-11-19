// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HostOffsets.h"
#include <string>
class BrowserSession;

struct ClassicFormatRequest {
 std::string path,label;
 hfv_size_t bytes;
};
bool StartClassicFormat(BrowserSession&,ClassicFormatRequest);
