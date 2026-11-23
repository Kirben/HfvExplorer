// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include "StorageOutcome.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <optional>
namespace Storage {
enum class ConflictChoice {None,Replace,ReplaceAll,Skip,Cancel};
enum class CopyPrompt {Overwrite,Uncompressed,IndependentFolders,ClassicMetadataLoss,ClassicArchiveNames,ClassicArchiveMove,DiscardClassicArchive,ArchiveFolderMerge};
struct PendingCopyPrompt {
 uint64_t id=0;
 std::string path;
 CopyPrompt kind=CopyPrompt::Overwrite;
};
struct Progress {
 std::string path,phase="Preparing",detail;
 unsigned long long bytes=0,processed=0,fileBytes=0,fileTotal=0,totalBytes=0;
 unsigned files=0,skipped=0,totalFiles=0;
 bool scanning=true,fileCopy=false,byteTransfer=false,utf8=false;
 bool timestampPrecisionReduced=false;
 bool sparseHolesAllocated=false;
 bool apfsCloneCopiedIndependently=false;
};
struct Result {
 int code=0;
 DWORD error=0;
 std::string message;
 bool moved=false;
 Outcome outcome=Outcome::Succeeded;
 Rollback rollback=Rollback::None;
 unsigned completed=0, skipped=0;
 bool partial=false;
 bool timestampPrecisionReduced=false;
 bool sparseHolesAllocated=false;
 bool apfsCloneCopiedIndependently=false;

 void Finalize(const OutcomeEvidence& evidence, unsigned items, unsigned skippedItems) {
  if(items>completed)completed=items;
  if(skippedItems>skipped)skipped=skippedItems;
  if(evidence.RollbackState()>rollback)rollback=evidence.RollbackState();
  outcome=error==ERROR_CANCELLED ? Outcome::Cancelled : code ? Outcome::Failed :
          skipped ? Outcome::Skipped : Outcome::Succeeded;
  partial=partial || (outcome!=Outcome::Succeeded && (completed || evidence.RetainedChanges()));
 }
};
struct MediaChanged : std::runtime_error { using std::runtime_error::runtime_error; };
class Cancelled : public std::exception {public:const char* what()const noexcept override{return "Storage job cancelled";}};
class Job {
 mutable std::mutex mutex_;
 std::condition_variable condition_;
 Progress progress_;Result result_;bool done_=false;
 PendingCopyPrompt prompt_;
 ConflictChoice choice_=ConflictChoice::None;bool replaceAll_=false,cancelled_=false;
 HANDLE event_;
 std::function<void(const Progress&)> report_;
 std::function<ConflictChoice(const std::string&)> ask_;
 std::function<void()> completion_;
public:
 Job():event_(CreateEvent(NULL,TRUE,FALSE,NULL)) {if(!event_)throw std::runtime_error("Could not create job completion event");}
 ~Job(){CloseHandle(event_);}
 HANDLE event()const{return event_;}
 Progress progress()const{std::lock_guard<std::mutex> lock(mutex_);return progress_;}
 void transport(std::function<void(const Progress&)> report,std::function<ConflictChoice(const std::string&)> ask){report_=std::move(report);ask_=std::move(ask);}
 void progress(const Progress& value){{std::lock_guard<std::mutex> lock(mutex_);progress_=value;}if(report_)report_(value);}
 bool done()const{std::lock_guard<std::mutex> lock(mutex_);return done_;}
 Result result()const{std::lock_guard<std::mutex> lock(mutex_);return result_;}
 // One completion observer, invoked outside the job lock. Late observers
 // receive the same notification even if the job has already finished.
 void whenFinished(std::function<void()> notify){
  {
   std::lock_guard<std::mutex> lock(mutex_);
   if(!done_){completion_=std::move(notify);return;}
  }
  notify();
 }
 void finish(Result result){
  std::function<void()> notify;
  {
   std::lock_guard<std::mutex> lock(mutex_);
   OutcomeEvidence evidence;
   result.Finalize(evidence,0,0);
   result.timestampPrecisionReduced|=progress_.timestampPrecisionReduced;
   result.sparseHolesAllocated|=progress_.sparseHolesAllocated;
   result.apfsCloneCopiedIndependently|=progress_.apfsCloneCopiedIndependently;
   result_=std::move(result);
   done_=true;
   notify=std::move(completion_);
  }
  SetEvent(event_);
  if(notify)notify();
 }
 void wait()const{WaitForSingleObject(event_,INFINITE);}
 std::string conflict()const{std::lock_guard<std::mutex> lock(mutex_);return prompt_.path;}
 PendingCopyPrompt conflictState()const{std::lock_guard<std::mutex> lock(mutex_);return prompt_;}
 void resolve(uint64_t id,ConflictChoice choice){{std::lock_guard<std::mutex> lock(mutex_);if(id!=prompt_.id||prompt_.path.empty())return;choice_=cancelled_?ConflictChoice::Cancel:choice;}condition_.notify_all();}
 bool cancelled()const{std::lock_guard<std::mutex> lock(mutex_);return cancelled_;}
 void cancel(){ {std::lock_guard<std::mutex> lock(mutex_);cancelled_=true;choice_=ConflictChoice::Cancel;}condition_.notify_all();}
 void resolve(ConflictChoice choice){ {std::lock_guard<std::mutex> lock(mutex_);choice_=cancelled_?ConflictChoice::Cancel:choice;}condition_.notify_all();}
 ConflictChoice replace(const std::string& path) {return requestChoice(CopyPrompt::Overwrite,path);}
 ConflictChoice requestChoice(CopyPrompt kind,const std::string& path) {
  const bool overwrite=kind==CopyPrompt::Overwrite;
  if(overwrite&&ask_)return ask_(path);
  std::unique_lock<std::mutex> lock(mutex_);
  if(cancelled_)return ConflictChoice::Cancel;
  if(overwrite&&replaceAll_)return ConflictChoice::Replace;
  prompt_={prompt_.id+1,path,kind};choice_=ConflictChoice::None;
  condition_.wait(lock,[&]{return choice_!=ConflictChoice::None;});
  ConflictChoice result=choice_;prompt_.path.clear();
  if(overwrite&&result==ConflictChoice::ReplaceAll){replaceAll_=true;return ConflictChoice::Replace;}
  if(result==ConflictChoice::Cancel)cancelled_=true;
  return result;
 }
};
class Worker {
 using Work=std::function<Result(Job&)>;
 using Ready=std::function<bool(const std::shared_ptr<Job>&)>;
 using Prepare=std::function<void(Job&)>;
 struct Entry {
  std::shared_ptr<Job> job;
  Work work;
  Ready ready;
  Prepare prepare;
  void releaseCallbacks(){prepare=nullptr;ready=nullptr;work=nullptr;}
 };
 std::mutex mutex_;std::condition_variable condition_;std::deque<Entry> queue_;bool stopping_=false;
 std::vector<std::thread> threads_;
 void loop() {
  for(;;) {
   Entry entry;
   std::optional<Result> readinessError;
   {
    std::unique_lock<std::mutex> lock(mutex_);
    for(;;){
     if(queue_.empty()&&stopping_)return;
     auto found=queue_.end();
     for(auto it=queue_.begin();it!=queue_.end();++it){
      try{if(it->prepare||!it->ready||it->ready(it->job)){found=it;break;}}
      catch(const std::exception& error){readinessError=Result{1,ERROR_UNHANDLED_EXCEPTION,error.what()};found=it;break;}
     }
     if(found!=queue_.end()){entry=std::move(*found);queue_.erase(found);break;}
     // Conflicting requests do not occupy workers or hold a partial lease.
     condition_.wait_for(lock,std::chrono::milliseconds(10));
    }
   }
   if(readinessError){entry.releaseCallbacks();entry.job->finish(std::move(*readinessError));continue;}
   if(entry.job->done())continue;
   Result result;
   OutcomeEvidence evidence;
   try {
    if(entry.prepare){
     auto prepare=std::move(entry.prepare);
     entry.prepare=nullptr;
     prepare(*entry.job);
     prepare=nullptr;
     // Preparation may do I/O. Publish the prepared entry under the
     // scheduler lock only after that I/O is complete.
     {
      std::lock_guard<std::mutex> lock(mutex_);
      if(stopping_)entry.job->cancel();
      queue_.push_front(std::move(entry));
     }
     condition_.notify_all();
     continue;
    }
    result=entry.work(*entry.job);
   }
   catch(const Cancelled&){result={1,ERROR_CANCELLED,"Cancelled"};}
   catch(const MediaChanged& error){result={1,ERROR_MEDIA_CHANGED,error.what()};}
   catch(const std::exception& error){result={1,ERROR_UNHANDLED_EXCEPTION,error.what()};}
   catch(...){result={1,ERROR_UNHANDLED_EXCEPTION,"Unexpected storage error"};}
   // Completion can immediately reopen the same storage. Drop the queued
   // request and its mount references before notifying callers.
   entry.releaseCallbacks();
   const auto progress=entry.job->progress();
   result.Finalize(evidence,progress.files,progress.skipped);
   entry.job->finish(std::move(result));condition_.notify_all();
  }
 }
public:
 explicit Worker(unsigned count=1){try{for(unsigned i=0;i<count;++i)threads_.emplace_back([this]{loop();});}catch(...){shutdown();throw;}}
 void shutdown(){ {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;for(auto& e:queue_)e.job->cancel();}condition_.notify_all();for(auto& thread:threads_)if(thread.joinable())thread.join();}
 ~Worker(){shutdown();}
 std::shared_ptr<Job> submit(Work work,Ready ready={}) {
  return submitPrepared({},std::move(ready),std::move(work));
 }
 std::shared_ptr<Job> submitPrepared(Prepare prepare,Ready ready,Work work) {
  auto job=std::make_shared<Job>();
  {std::lock_guard<std::mutex> lock(mutex_);if(stopping_)throw std::runtime_error("Storage worker is shutting down");queue_.push_back({job,std::move(work),std::move(ready),std::move(prepare)});}
  condition_.notify_all();return job;
 }
};
inline Worker& worker(){static Worker value;return value;}
inline Worker& navigationWorker(){static Worker value(4);return value;}
inline Worker& copyWorker(){static Worker value(4);return value;}
}
