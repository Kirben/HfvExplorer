// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
namespace Storage {
enum class ConflictChoice {None,Replace,ReplaceAll,Skip,Cancel};
struct Progress {
 std::string path,phase="Preparing",detail;
 unsigned long long bytes=0,processed=0,fileBytes=0,fileTotal=0,totalBytes=0;
 unsigned files=0,skipped=0,totalFiles=0;
 bool scanning=true,fileCopy=false,byteTransfer=false,utf8=false;
};
struct Result {int code=0;DWORD error=0;std::string message;bool moved=false;};
class Cancelled : public std::exception {public:const char* what()const noexcept override{return "Storage job cancelled";}};
class Job {
 mutable std::mutex mutex_;
 std::condition_variable condition_;
 Progress progress_;Result result_;bool done_=false;
 std::string conflict_;ConflictChoice choice_=ConflictChoice::None;bool replaceAll_=false,cancelled_=false;
 HANDLE event_;
public:
 Job():event_(CreateEvent(NULL,TRUE,FALSE,NULL)) {if(!event_)throw std::runtime_error("Could not create job completion event");}
 ~Job(){CloseHandle(event_);}
 HANDLE event()const{return event_;}
 Progress progress()const{std::lock_guard<std::mutex> lock(mutex_);return progress_;}
 void progress(const Progress& value){std::lock_guard<std::mutex> lock(mutex_);progress_=value;}
 bool done()const{std::lock_guard<std::mutex> lock(mutex_);return done_;}
 Result result()const{std::lock_guard<std::mutex> lock(mutex_);return result_;}
 void finish(Result result){ {std::lock_guard<std::mutex> lock(mutex_);result_=std::move(result);done_=true;}SetEvent(event_);}
 void wait()const{WaitForSingleObject(event_,INFINITE);}
 std::string conflict()const{std::lock_guard<std::mutex> lock(mutex_);return conflict_;}
 bool cancelled()const{std::lock_guard<std::mutex> lock(mutex_);return cancelled_;}
 void cancel(){ {std::lock_guard<std::mutex> lock(mutex_);cancelled_=true;choice_=ConflictChoice::Cancel;}condition_.notify_all();}
 void resolve(ConflictChoice choice){ {std::lock_guard<std::mutex> lock(mutex_);choice_=cancelled_?ConflictChoice::Cancel:choice;}condition_.notify_all();}
 ConflictChoice replace(const std::string& path) {
  std::unique_lock<std::mutex> lock(mutex_);
  if(cancelled_)return ConflictChoice::Cancel;
  if(replaceAll_)return ConflictChoice::Replace;
  conflict_=path;choice_=ConflictChoice::None;
  condition_.wait(lock,[&]{return choice_!=ConflictChoice::None;});
  ConflictChoice result=choice_;conflict_.clear();
  if(result==ConflictChoice::ReplaceAll){replaceAll_=true;return ConflictChoice::Replace;}
  if(result==ConflictChoice::Cancel)cancelled_=true;
  return result;
 }
};
class Worker {
 struct Entry {std::shared_ptr<Job> job;std::function<Result(Job&)> work;};
 std::mutex mutex_;std::condition_variable condition_;std::deque<Entry> queue_;bool stopping_=false;
 std::thread thread_;
 void loop() {
  for(;;) {
   Entry entry;
   {std::unique_lock<std::mutex> lock(mutex_);condition_.wait(lock,[&]{return stopping_||!queue_.empty();});
    if(queue_.empty()&&stopping_)return;entry=std::move(queue_.front());queue_.pop_front();}
   Result result;
   try {result=entry.work(*entry.job);}
   catch(const Cancelled&){result={1,ERROR_CANCELLED,"Cancelled"};}
   catch(const std::exception& error){result.code=1;result.error=ERROR_UNHANDLED_EXCEPTION;result.message=error.what();}
   catch(...){result.code=1;result.error=ERROR_UNHANDLED_EXCEPTION;result.message="Unexpected storage error";}
   entry.job->finish(std::move(result));
  }
 }
public:
 Worker():thread_([this]{loop();}){}
 void shutdown(){ {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}condition_.notify_all();if(thread_.joinable())thread_.join();}
 ~Worker(){shutdown();}
 std::shared_ptr<Job> submit(std::function<Result(Job&)> work) {
  auto job=std::make_shared<Job>();
  {std::lock_guard<std::mutex> lock(mutex_);if(stopping_)throw std::runtime_error("Storage worker is shutting down");queue_.push_back({job,std::move(work)});}
  condition_.notify_one();return job;
 }
};
inline Worker& worker(){static Worker value;return value;}
inline Worker& copyWorker(){static Worker value;return value;}
}
