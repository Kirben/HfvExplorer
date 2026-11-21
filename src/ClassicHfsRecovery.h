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
#include <memory>
#include <string>
namespace ClassicHfsRecovery {
bool Pending(const std::wstring& path);
class Transaction {
    struct State;
    std::unique_ptr<State> state_;
public:
    static bool Supported(int fd);
    explicit Transaction(int fd);
    ~Transaction();
    bool Active() const;
    bool CommitStarted() const;
    void Commit();
    void Detach();
#ifdef HFV_TESTING
    static void CrashAt(unsigned phase);
#endif
};
}
#endif
