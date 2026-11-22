// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BackgroundJobController.h"

namespace BackgroundCopies {

JobController::JobController(std::vector<FileOperations::Request> requests,
    std::function<void(bool)> completed,
    std::function<void(const FileOperations::Completion&)> observed)
    : requests_(std::move(requests)), completed_(std::move(completed)), observed_(std::move(observed))
{}

JobController::JobController(Work work,std::function<void(const Storage::Result&)> completed)
    : work_(std::move(work)), storageCompleted_(std::move(completed)) {}

std::shared_ptr<Storage::Job> JobController::Job() const
{
    return operation_ ? operation_->Job() : task_;
}

const FileOperations::LegacyConversion* JobController::PendingMode() const
{
    if (cancelled_ || finished_ || next_ == requests_.size())
        return nullptr;
    const auto* pending = requests_[next_].Conversion();
    return pending && pending->askMode ? pending : nullptr;
}

void JobController::CollectResult()
{
    auto job = Job();
    auto previous = std::move(result_);
    result_ = job->result();
    result_.completed += previous.completed;
    result_.skipped += previous.skipped;
    result_.rollback = (std::max)(previous.rollback, result_.rollback);
    result_.partial = result_.partial || previous.partial;
    result_.timestampPrecisionReduced |= previous.timestampPrecisionReduced;
    result_.sparseHolesAllocated |= previous.sparseHolesAllocated;
    if (operation_) {
        snapshot_ = operation_->Snapshot();
        complete_ = complete_ && snapshot_.complete;
        cancelled_ = cancelled_ || job->cancelled();
        operation_.reset();
    } else {
        task_.reset();
        // A device request can complete after cancellation is requested.
        cancelled_ = result_.error == ERROR_CANCELLED;
    }
    complete_ = complete_ && !result_.code;
    cancelled_ = cancelled_ || result_.code;
}

JobController::Step JobController::Advance()
{
    if (finished_)
        return Step::Finished;
    if (work_ && !cancelled_) {
        task_ = Storage::copyWorker().submit([work=std::move(work_)](Storage::Job& job) mutable {
            auto run=std::move(work);
            return run(job);
        });
        work_ = {};
        return Step::Started;
    }
    if (auto job = Job()) {
        if (!job->done())
            return Step::Running;
        CollectResult();
    }
    if (cancelled_ || next_ == requests_.size())
        return Step::Finished;
    if (const auto* conversion = PendingMode()) {
        int direction = conversion->destination.IsWindows() ? 1 : 0;
        if (!all_[direction])
            return Step::ChooseMode;
        auto& pending = requests_[next_].Legacy();
        if (direction)
            pending.options.output = choices_[direction].output;
        else {
            pending.options.input = choices_[direction].input;
            pending.options.appledouble = choices_[direction].appledouble;
        }
        pending.askMode = false;
    }
    auto request = std::move(requests_[next_++]);
    request.background = true;
    operation_.reset(new FileOperations::Operation(FileOperations::Submit(std::move(request))));
    return Step::Started;
}

void JobController::ChooseMode(const ModeChoice& choice)
{
    const auto* conversion = PendingMode();
    if (!conversion)
        return;
    int direction = conversion->destination.IsWindows() ? 1 : 0;
    auto& pending = requests_[next_].Legacy();
    const char modes[] = "ambtrRx53";
    char value = choice.selection >= 0 && size_t(choice.selection) < sizeof(modes) - 1 ? modes[choice.selection] : 'a';
    if (direction)
        pending.options.output = value;
    else {
        pending.options.input = value;
        pending.options.appledouble = choice.appledouble;
    }
    all_[direction] = choice.applyToAll;
    choices_[direction] = pending.options;
    pending.askMode = false;
}

void JobController::Resolve(uint64_t conflict, Storage::ConflictChoice choice)
{
    if (auto job = Job(); job && choice != Storage::ConflictChoice::None)
        job->resolve(conflict, choice);
}

void JobController::Cancel()
{
    if (finished_)
        return;
    cancelled_ = true;
    if (auto job = Job())
        job->cancel();
}

void JobController::Fail(const char* message)
{
    Cancel();
    if (auto job = Job()) {
        job->wait();
        CollectResult();
    }
    result_.code = 1;
    result_.error = ERROR_UNHANDLED_EXCEPTION;
    result_.message = message;
}

void JobController::Finish()
{
    if (finished_)
        return;
    if (auto job = Job()) {
        job->wait();
        CollectResult();
    }
    if (cancelled_ && !result_.code) {
        result_.code = 1;
        result_.error = ERROR_CANCELLED;
        result_.message = "Operation cancelled";
    }
    Storage::OutcomeEvidence evidence;
    result_.Finalize(evidence, 0, 0);
    finished_ = true;
    bool success = complete_ && !cancelled_ && next_ == requests_.size();
    requests_.clear();
    work_ = {};
    auto completed = std::move(completed_);
    auto storageCompleted=std::move(storageCompleted_);
    auto observed = std::move(observed_);
    auto snapshot = std::move(snapshot_);
    if (completed)
        completed(success);
    if (success && observed)
        observed(snapshot);
    if(storageCompleted)storageCompleted(result_);
}

}
