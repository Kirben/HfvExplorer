// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <stdexcept>
#include <cstdint>
namespace MacBrowser {class Volume;}
namespace VolumeCoord {
std::wstring Wide(const std::string&);
std::string ImageKey(const std::wstring&);
inline std::string ImageKey(const std::string& path){return ImageKey(Wide(path));}
std::string HostKey(const std::wstring&);
std::string DeviceKey(int drive);
enum class Mode {Read,Write};
struct Access {std::string key;Mode mode=Mode::Read;};
class Busy:public std::runtime_error {public:Busy():std::runtime_error("This volume is already in use by another storage operation"){};};
// Atomic acquisition of all resources: no partial acquisition or lock ordering deadlocks.
class Lease {
 std::vector<Access> access_;
public:
 explicit Lease(std::vector<Access>);
 ~Lease();
 Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
};
bool Readable(const std::string&);
// Advisory command eligibility; workers must still acquire an atomic reservation.
bool Available(const std::vector<Access>&);
// Scoped command work may reuse a writer already owned by this worker thread.
class OperationLease {
 std::unique_ptr<Lease> lease_;
public:explicit OperationLease(std::vector<Access>);
};
// A backend call takes a read lease, or runs under its thread's existing writer.
class BackendRead {
 std::unique_ptr<Lease> lease_;
public:explicit BackendRead(const std::string&);
};
uint64_t Revision(const std::string&);
void Changed(const std::vector<std::string>&);
std::shared_ptr<MacBrowser::Volume> SharedImage(const std::string&,std::function<std::shared_ptr<MacBrowser::Volume>()>);
constexpr UINT Notification=WM_APP+117;
struct Event {std::string key;uint64_t revision=0;};
// The worker owns only this mailbox, never a browser pointer. Posts carry a
// subscription ID so stale messages cannot target a later/reused window.
class Subscription {
public:
 struct State;
private:std::shared_ptr<State> state_;
public:
 Subscription();~Subscription();
 Subscription(const Subscription&)=delete;Subscription& operator=(const Subscription&)=delete;
 void Bind(HWND);void Close();void Watch(const std::string&);
 uintptr_t Id()const;
 std::vector<Event> Drain();
};
}
