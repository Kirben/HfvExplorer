// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Storage {

enum class Outcome { Succeeded, Skipped, Cancelled, Failed };
// Reports the worst rollback disposition of any failed change in the job.
// A restored change does not undo earlier successful items in the same job.
enum class Rollback { None, Restored, Unverified, Failed };

class OutcomeEvidence {
    static OutcomeEvidence*& Current() {
        static thread_local OutcomeEvidence* evidence = nullptr;
        return evidence;
    }
    OutcomeEvidence* previous_ = Current();
    Rollback rollback_ = Rollback::None;
    bool retained_ = false;
public:
    OutcomeEvidence() { Current() = this; }
    ~OutcomeEvidence() { Current() = previous_; }
    OutcomeEvidence(const OutcomeEvidence&) = delete;
    OutcomeEvidence& operator=(const OutcomeEvidence&) = delete;

    Rollback RollbackState() const { return rollback_; }
    bool RetainedChanges() const { return retained_; }

    static void NoteRollback(Rollback state) {
        if (Current() && state > Current()->rollback_) Current()->rollback_ = state;
    }
    static void NoteRetainedChange() {
        if (Current()) Current()->retained_ = true;
    }
};

} // namespace Storage
