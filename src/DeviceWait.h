// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
namespace StorageUi {
typedef void (*WaitFunction)(const std::shared_ptr<Storage::Job>&);
struct State {DWORD thread=0;unsigned active=0;bool sessionOpen=false;WaitFunction wait=nullptr;};
inline State& state(){static State value;return value;}
inline bool busy(){return GetCurrentThreadId()==state().thread && state().active!=0;}
inline void initialize(WaitFunction wait){state().thread=GetCurrentThreadId();state().wait=wait;}
}
namespace HfvDeviceWait {
inline bool busy(){return StorageUi::busy();}
template<class Call> auto invoke(Call call,decltype(call()) failure)->decltype(call()) {
 typedef decltype(call()) Result;
 if(GetCurrentThreadId()!=StorageUi::state().thread || !StorageUi::state().wait)return call();
 if(busy()){SetLastError(ERROR_BUSY);return failure;}
 Result value={};DWORD error=0;
 auto job=Storage::worker().submit([&](Storage::Job& current)->Storage::Result {Storage::Progress progress;progress.phase="Reading volume";current.progress(progress);value=call();error=GetLastError();return {};});
 struct Lifetime {std::shared_ptr<Storage::Job> job;~Lifetime(){job->wait();}} lifetime={job};
 if(StorageUi::state().sessionOpen || WaitForSingleObject(job->event(),40)!=WAIT_OBJECT_0)StorageUi::state().wait(job);
 job->wait();
 if(job->result().code){SetLastError(job->result().error);return failure;}
 SetLastError(error);return value;
}
template<class Call> auto invoke(Call call)->decltype(call()) {typedef decltype(call()) Result;return invoke(call,Result());}
}
inline HANDLE HfvMediaOpen(LPCSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD f,HANDLE h){
 return HfvDeviceWait::invoke([&]{return CreateFileA(p,a,s,sa,d,f,h);},INVALID_HANDLE_VALUE);}
inline BOOL HfvMediaControl(HANDLE h,DWORD c,LPVOID i,DWORD n,LPVOID o,DWORD m,LPDWORD r,LPOVERLAPPED l){
 return HfvDeviceWait::invoke([&]{return DeviceIoControl(h,c,i,n,o,m,r,l);});}
inline BOOL HfvMediaRead(HANDLE h,LPVOID p,DWORD n,LPDWORD r,LPOVERLAPPED l){
 return HfvDeviceWait::invoke([&]{return ReadFile(h,p,n,r,l);});}
inline BOOL HfvMediaSeek(HANDLE h,LARGE_INTEGER p,PLARGE_INTEGER r,DWORD o){
 return HfvDeviceWait::invoke([&]{return SetFilePointerEx(h,p,r,o);});}
inline BOOL HfvMediaClose(HANDLE h){return HfvDeviceWait::invoke([&]{return CloseHandle(h);});}