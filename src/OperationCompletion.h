// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MediaWriteBackend.h"
#include "StorageWorker.h"
#include "VolumeCoordinator.h"

extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}

namespace Storage {

struct OperationError : HfsPlus::Error {
    DWORD error;
    OperationError(DWORD code, const std::string& message)
        : HfsPlus::Error(message), error(code ? code : ERROR_WRITE_FAULT) {}
};

// Each executor owns its evidence until the operation has completed. Worker
// replies merge their evidence into the caller's scope before it is finalized.
class OperationCompletion {
    OutcomeEvidence evidence_;

public:
    OperationCompletion()
    {
        hfs_take_rollback();
        HFSIFACE_take_delete_retained_change();
    }

    template<class Work>
    Result Run(Work work, DWORD unexpectedCode = ERROR_UNHANDLED_EXCEPTION,
               const char* cancelled = "Cancelled")
    {
        try { return work(); }
        catch (const Cancelled&) { return {1, ERROR_CANCELLED, cancelled}; }
        catch (const MediaChanged& error) { return {1, ERROR_MEDIA_CHANGED, error.what()}; }
        catch (const MediaIoError& error) { return {1, error.code, error.what()}; }
        catch (const OperationError& error) { return {1, error.error, error.what()}; }
        catch (const VolumeCoord::Busy& error) { return {1, ERROR_BUSY, error.what()}; }
        catch (const std::exception& error) { return {1, unexpectedCode, error.what()}; }
        catch (...) { return {1, unexpectedCode, "Unexpected file-operation failure"}; }
    }

    void Finish(Result& result, unsigned completed = 0, unsigned skipped = 0)
    {
        OutcomeEvidence::NoteRollback(static_cast<Rollback>(hfs_take_rollback()));
        if (HFSIFACE_take_delete_retained_change()) OutcomeEvidence::NoteRetainedChange();
        result.Finalize(evidence_, completed, skipped);
    }
};

} // namespace Storage
