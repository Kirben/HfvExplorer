// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BrowserPreview.h"
#include "StorageLocation.h"
#include "VolumeCoordinator.h"

namespace BrowserPreview {
namespace {
std::wstring Convert(UINT codePage, DWORD flags, const uint8_t* bytes, size_t length) {
    const auto input = reinterpret_cast<const char*>(bytes);
    const auto count = MultiByteToWideChar(codePage, flags, input, int(length), nullptr, 0);
    if (!count) return {};
    std::wstring output(count, L'\0');
    if (!MultiByteToWideChar(codePage, flags, input, int(length), output.data(), count)) return {};
    return output;
}

std::wstring Utf8(const uint8_t* bytes, size_t length, bool truncated) {
    if (truncated && length) {
        size_t last = length - 1;
        while (last && length - last < 4 && (bytes[last] & 0xc0) == 0x80) --last;
        const auto lead = bytes[last];
        const size_t sequence = lead >= 0xf0 && lead <= 0xf4 ? 4 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xc2 && lead <= 0xdf ? 2 : 0;
        if (sequence && length - last < sequence) length = last;
    }
    return Convert(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length);
}
}

Text Decode(const std::vector<uint8_t>& bytes, bool mac, bool finderText, bool truncated) {
    Text result;
    result.truncated = truncated;
    if (bytes.empty()) {
        result.available = true;
        return result;
    }
    for (auto byte : bytes)
        if ((byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t') || byte == 0x7f)
            return result;

    const bool bom = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    std::wstring text;
    if (bom || !mac || !finderText) {
        text = Utf8(bytes.data() + (bom ? 3 : 0), bytes.size() - (bom ? 3 : 0), truncated);
        if (!text.empty() || (bom && bytes.size() == 3)) result.encoding = L"UTF-8";
    }
    if (result.encoding.empty() && (mac || finderText) && !bom) {
        text = Convert(10000, 0, bytes.data(), bytes.size());
        if (!text.empty()) result.encoding = L"MacRoman";
    }
    if (result.encoding.empty()) return result;
    result.available = true;
    // Native multiline edits use CRLF for all line breaks.
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r' || text[i] == L'\n') {
            if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            result.content += L"\r\n";
        } else result.content += text[i];
    }
    return result;
}

void Request::Start(FileOperations::Location item, std::string key, bool mac, bool finderText) {
    Cancel();
    key_ = std::move(key);
    revision_ = VolumeCoord::Revision(key_);
    result_ = std::make_shared<Text>();
    // Slow media previews must not occupy the folder navigation queue.
    static Storage::Worker reader(2);
    job_ = reader.submit([item = std::move(item), result = result_, mac, finderText](Storage::Job& job) {
        try {
            bool truncated = false;
            const auto bytes = StorageLocation::ReadDataPrefix(item, SampleLimit, truncated, job);
            *result = Decode(bytes, mac, finderText, truncated);
            return Storage::Result{};
        } catch (const VolumeCoord::Busy& error) {
            return Storage::Result{1, ERROR_BUSY, error.what()};
        }
    });
}

void Request::Cancel() {
    if (job_) job_->cancel();
    job_.reset();
    result_.reset();
    key_.clear();
}

bool Request::Stale() const {
    return !StorageLocation::RevisionCurrent(key_,revision_);
}

Request::State Request::Take(Text& text) {
    if (!job_ || !job_->done()) return State::Pending;
    const auto status = job_->result();
    job_.reset();
    auto result = std::move(result_);
    if (Stale() || status.error == ERROR_MEDIA_CHANGED) return State::Stale;
    if (status.error == ERROR_BUSY) return State::Busy;
    if (status.code) return State::Failed;
    if (!result->available) return State::Unavailable;
    text = std::move(*result);
    return State::Ready;
}
}
