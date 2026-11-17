// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "hfs/EngineContext.h"
#include <memory>
#include <mutex>
#include <map>
#include <string>
extern "C" void hfs_umountall(void);
namespace LegacyEngine {
struct Context {
 hfs_engine_context state{};
 std::recursive_mutex mutex;
 ~Context(){auto previous=hfs_context_use(&state);hfs_umountall();hfs_context_use(previous);}
};
inline std::weak_ptr<Context>& Active(){static thread_local std::weak_ptr<Context> active;return active;}
// A persistent classic adapter shares mount caches only with aliases of its
// own volume. The registry lock is never held during filesystem/device I/O.
inline std::shared_ptr<Context> ForVolume(const std::string& key){
 static std::mutex mutex;static std::map<std::string,std::weak_ptr<Context>> contexts;
 std::lock_guard<std::mutex> lock(mutex);auto value=contexts[key].lock();if(!value){value=std::make_shared<Context>();contexts[key]=value;}return value;
}
// Only callers using the same context serialize. Unrelated jobs own distinct contexts.
class Scope {
 std::shared_ptr<Context> context_;
 std::weak_ptr<Context> previousOwner_;
 std::unique_lock<std::recursive_mutex> lock_;
 hfs_engine_context *previous_;
public:
 explicit Scope(std::shared_ptr<Context> value=std::make_shared<Context>()):context_(std::move(value)),previousOwner_(Active()),lock_(context_->mutex),previous_(hfs_context_use(&context_->state)){Active()=context_;}
 ~Scope(){hfs_context_use(previous_);Active()=previousOwner_;}
};
}
