// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HostOffsets.h"
#include <string>
class BrowserSession;
namespace Storage { class Job; }
void FormatClassicImage(const std::wstring& path,std::string label,hfv_size_t bytes,
                        Storage::Job* job=nullptr);
#ifdef HFV_TESTING
#include <functional>
namespace HfsPlus { class Source; }
enum class ClassicFormatStage { Verifying, Publishing, Published };
extern std::function<void(ClassicFormatStage,HfsPlus::Source&)> classicFormatTestStage;
#endif

struct ClassicFormatRequest {
 std::string path,label;
 hfv_size_t bytes;
};
bool StartClassicFormat(BrowserSession&,ClassicFormatRequest);
