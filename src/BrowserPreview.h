// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileLocation.h"

namespace BrowserPreview {
constexpr size_t SampleLimit = 16 * 1024;
struct Text {
    std::wstring content, encoding;
    bool available = false, truncated = false;
};
Text Decode(const std::vector<uint8_t>&, bool mac, bool finderText, bool truncated);

// A pane owns one request. A superseded job may finish in the worker,
// but only the current request can publish to the pane.
class Request {
    std::shared_ptr<Storage::Job> job_;
    std::shared_ptr<Text> result_;
    std::string key_;
    uint64_t revision_ = 0;
public:
    ~Request() { Cancel(); }
    void Start(FileOperations::Location, std::string key, bool mac, bool finderText);
    void Cancel();
    bool Pending() const { return bool(job_); }
    bool Stale() const;
    enum class State { Pending, Ready, Unavailable, Busy, Failed, Stale };
    State Take(Text&);
};
}
