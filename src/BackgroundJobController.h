// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"

namespace BackgroundCopies {

class JobController {
public:
    enum class Step { Running, Started, ChooseMode, Finished };
    struct ModeChoice {
        int selection;
        bool appledouble;
        bool applyToAll;
    };

    JobController(std::vector<FileOperations::Request> requests,
        std::function<void(bool)> completed = {},
        std::function<void(const FileOperations::Completion&)> observed = {});
    Step Advance();
    const FileOperations::LegacyConversion* PendingMode() const;
    void ChooseMode(const ModeChoice&);
    void Resolve(uint64_t conflict, Storage::ConflictChoice choice);
    void Cancel();
    void Fail(const char* message);
    void Finish();
    bool Finished() const { return finished_; }
    std::shared_ptr<Storage::Job> Job() const;
    const Storage::Result& Result() const { return result_; }

private:
    void CollectResult();
    std::vector<FileOperations::Request> requests_;
    size_t next_ = 0;
    std::unique_ptr<FileOperations::Operation> operation_;
    std::function<void(bool)> completed_;
    std::function<void(const FileOperations::Completion&)> observed_;
    FileOperations::Completion snapshot_;
    Storage::Result result_;
    bool complete_ = true, cancelled_ = false, finished_ = false;
    bool all_[2]{};
    LegacyCopyOptions choices_[2];
};

}
