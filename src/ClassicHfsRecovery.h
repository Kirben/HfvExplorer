/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Returns -1 on failure, 0 when no undo is pending, and 1 after recovery. */
int hfv_hfs_recover_image(int fd,int writable);
int hfv_hfs_before_sector_write(int fd,unsigned long long offset);
void hfv_hfs_after_sector_write(int fd);
#ifdef HFV_TESTING
void hfv_hfs_recovery_checkpoint(unsigned phase);
#endif
#ifdef __cplusplus
}
#include <cstdint>
#include <memory>
#include <string>
namespace ClassicHfsRecovery {
// One-to-one sector mapping into a held, ordinary NDIF data file.
struct ImageRange {
    uintptr_t handle;
    uint64_t offset;
    uint64_t length;
};
bool Pending(const std::wstring& path,bool ndif=false);
bool RecoverNdif(const ImageRange&,bool writable,bool keepJournal=false);
inline constexpr size_t MoveIdSize=32;
std::wstring MoveDecisionPath(const std::string& id,bool create=false);
bool MoveIsCommitted(const std::string& id);
void ForgetMoveDecision(const std::string& id) noexcept;
class MoveDecision {
    std::string id_;
public:
    MoveDecision();
    const std::string& Id() const { return id_; }
    void Commit();
    void Forget() noexcept;
};
class Transaction {
    struct State;
    std::unique_ptr<State> state_;
public:
    static bool Supported(int fd,const ImageRange* ndif=nullptr);
    explicit Transaction(int fd,const ImageRange* ndif=nullptr,const std::string& move={});
    ~Transaction();
    bool Active() const;
    bool CommitStarted() const;
    bool PublishedMove() const;
    void Prepare();
    void Commit();
    void Detach();
#ifdef HFV_TESTING
    static void CrashAt(unsigned phase);
#endif
};
}
#endif
