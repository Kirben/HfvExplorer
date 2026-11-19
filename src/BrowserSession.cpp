// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "StorageLocation.h"
#include "MainMacBrowser.h"
#include "BackgroundCopies.h"
#include "DeviceWait.h"
#include "StorageJobDialog.h"
#include "BrowserPresentation.h"
#include "BrowserDirectory.h"
#include "charset.h"
#include "FindFiles.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
}
#include <stdexcept>
#include <algorithm>
#include <set>
#include <fcntl.h>
struct BrowserSession::Notifications:public CWnd {
 BrowserSession& session;
 explicit Notifications(BrowserSession& value):session(value){if(!CreateEx(0,AfxRegisterWndClass(0),"HFV volume notifications",0,0,0,0,0,HWND_MESSAGE,nullptr))throw std::runtime_error("Cannot create volume notification mailbox");}
 ~Notifications(){if(GetSafeHwnd())DestroyWindow();}
 LRESULT WindowProc(UINT message,WPARAM w,LPARAM l)override{
  if(message==WM_TIMER&&w==4){
   if(!StorageUi::busy()&&!StorageUi::state().sessionOpen&&!HfvDeviceWait::busy()&&
      !session.MacBrowser().ApplyMediaChanges())KillTimer(4);
   return 0;
  }
  if(message==WM_TIMER&&w==3){session.CompleteNavigation();return 0;}
  if(message==WM_TIMER&&w==2){
   if(!session.owner_||!session.tree_||!session.list_){session.deferred_={};KillTimer(2);return 0;}
   if(StorageUi::busy()||StorageUi::state().sessionOpen||(session.deferredClassic_&&session.deferredKey_.empty()&&BackgroundCopies::Active())||!VolumeCoord::Readable(session.deferredKey_))return 0;
   KillTimer(2);auto next=std::move(session.deferred_);session.deferred_={};if(next)next();return 0;
  }
  if((message==VolumeCoord::Notification&&w==session.subscription_.Id())||(message==WM_TIMER&&w==1)){
   auto list=session.List();bool editing=list&&IsWindow(list->GetSafeHwnd())&&(list->GetListCtrl().GetEditControl()||list->IsDragging());
   if(StorageUi::busy()||StorageUi::state().sessionOpen||session.refreshing_||session.Navigating()||editing||(!session.current_.key.empty()&&!VolumeCoord::Available({{session.current_.key,session.current_.folder.IsWindows()?VolumeCoord::Mode::Browse:VolumeCoord::Mode::Read}}))){SetTimer(1,75,nullptr);return 0;}
   KillTimer(1);session.ProcessChanges();return 0;
  }
  return CWnd::WindowProc(message,w,l);
 }
};
BrowserSession::BrowserSession(CHFVExplorerDoc* doc):document_(doc),browser_(new MainMacBrowser(*this)){
    finderLayout=static_cast<CHFVExplorerApp*>(AfxGetApp())->m_mac_window_mode!=FALSE;
}
namespace {
const wchar_t* CaptionProcedure=L"HFVExplorer.UnicodeCaption";
LRESULT CALLBACK UnicodeCaption(HWND window,UINT message,WPARAM w,LPARAM l){
 auto previous=reinterpret_cast<WNDPROC>(GetPropW(window,CaptionProcedure));
 // Keep the legacy MFC command/view plumbing ANSI, but handle caption strings
 // with the Unicode default procedure so Mac names survive unchanged.
 if(message==WM_SETTEXT||message==WM_GETTEXT||message==WM_GETTEXTLENGTH)return DefWindowProcW(window,message,w,l);
 if(message==WM_NCDESTROY)RemovePropW(window,CaptionProcedure);
 return previous?CallWindowProcA(previous,window,message,w,l):DefWindowProcW(window,message,w,l);
}
}
void BrowserSession::UpdateWindowTitle()const{
 if(!owner_||!IsWindow(owner_->GetSafeHwnd()))return;HWND window=owner_->GetSafeHwnd();
 if(!IsWindowUnicode(window)&&!GetPropW(window,CaptionProcedure)){
  auto previous=reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window,GWLP_WNDPROC));
  if(SetPropW(window,CaptionProcedure,reinterpret_cast<HANDLE>(previous))){SetLastError(0);if(!SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(UnicodeCaption))&&GetLastError())RemovePropW(window,CaptionProcedure);}
 }
 auto caption=WindowTitle();if(BrowserPresentation::Text(window)!=caption)::SetWindowTextW(window,caption.c_str());
}
std::wstring BrowserSession::WindowTitle()const{return locationTitle_.empty()?L"HFVExplorer":locationTitle_+L" \u2014 HFVExplorer";}
void BrowserSession::SetLocationTitle(std::wstring title){
 locationTitle_=std::move(title);UpdateWindowTitle();
}
std::string BrowserSession::ObservedWindowsKey(const std::wstring& path)const{return current_.folder.IsWindows()&&!_wcsicmp(current_.folder.WindowsPath().c_str(),path.c_str())?current_.key:std::string();}
void BrowserSession::ClearCurrent(){CancelNavigation();if(preview_)preview_->Clear();listingPending_=false;current_=Context{};deferred_={};if(notifications_)notifications_->KillTimer(2);}
bool BrowserSession::UsesAsyncNavigation() const
{
 return owner_!=nullptr;
}

#ifdef HFV_TESTING
bool BrowserSession::AwaitNavigationForTest()
{
 const auto started=GetTickCount64();
 while(Navigating()){
  if(GetTickCount64()-started>15000)
   throw std::runtime_error("Timed out waiting for browser navigation");
  MSG message{};
  while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)){
   if(message.message==WM_QUIT){
    PostQuitMessage(int(message.wParam));
    throw std::runtime_error("Browser quit while waiting for navigation");
   }
   TranslateMessage(&message);
   DispatchMessage(&message);
  }
  Sleep(1);
 }
 return navigationErrorForTest.empty();
}
#endif

void BrowserSession::ScheduleMediaUpdates()
{
 if(notifications_)notifications_->SetTimer(4,60,nullptr);
}

void BrowserSession::CancelNavigation()
{
    navigationMediaDrive_=-1;
    deferred_ = {};
    if (notifications_)notifications_->KillTimer(2);
    if (!navigation_)
        return;
    navigation_->cancel();
    navigation_.reset();
    navigationReady_ = {};
    if (notifications_)
        notifications_->KillTimer(3);
}

namespace {
bool SameFolder(const FileOperations::Location& a,const FileOperations::Location& b)
{
 if(a.IsMounted()&&b.IsMounted())return a.Volume()==b.Volume()&&a.Id()==b.Id();
 if(a.IsClassic()&&b.IsClassic())return a.expectedCatalogId==b.expectedCatalogId;
 return a.IsWindows()&&b.IsWindows()&&!_wcsicmp(a.WindowsPath().c_str(),b.WindowsPath().c_str());
}

bool MatchesNavigationEntry(const BrowserDirectory::Row& row,const FileOperations::Location& item)
{
 if(!StorageLocation::MatchesEntry(item,row.entry))return false;
 const auto& location=row.location;
 if(item.MediaGenerationKnown()&&(!location.MediaGenerationKnown()||location.MediaGeneration()!=item.MediaGeneration()))return false;
 if(item.WindowsMediaSerial()&&location.WindowsMediaSerial()!=item.WindowsMediaSerial())return false;
 if(item.IsMounted())return location.IsMounted()&&location.Id()==item.Id();
 if(item.IsClassic())return location.IsClassic()&&!_stricmp(location.ClassicPath().c_str(),item.ClassicPath().c_str());
 return item.IsWindows()&&location.IsWindows()&&!_wcsicmp(location.WindowsPath().c_str(),item.WindowsPath().c_str());
}
}

void BrowserSession::Navigate(BrowserNavigation::Request request, BrowserNavigation::Read read,
    BrowserNavigation::Publish publish, std::optional<BrowserMountInventory::Entry> mount, std::function<void()> retry,
    std::function<bool()> nextOnError)
{
    const bool openingImage=request.image.has_value();
    const bool previousListing=ListingAvailable();
    const bool restoreOnError=request.intent==BrowserNavigation::Intent::OpenImage&&previousListing;
    CancelNavigation();
    if(preview_)preview_->Clear();
    navigationMediaDrive_=request.mediaDrive;
#ifdef HFV_TESTING
    navigationErrorForTest.clear();
#endif
    const auto anchor=tree_->GetTreeCtrl().GetSelectedItem();
    auto result=std::make_shared<BrowserNavigation::Result>();
    result->listing.folder=request.folder;

    std::vector<FileOperations::Location> selection;
    if (request.intent==BrowserNavigation::Intent::RefreshFolder &&
        current_.key==request.resource &&
        SameFolder(current_.folder,request.folder)) {
        selection=CaptureListingSelection();
    }

    navigation_=Storage::navigationWorker().submit(
        [request,result,read=std::move(read)](Storage::Job& job){return read(request,*result,job);});
    navigationReady_=[this,anchor,request=std::move(request),result,publish=std::move(publish),
                      mount=std::move(mount),retry=std::move(retry),
                      selection=std::move(selection),restoreOnError,nextOnError=std::move(nextOnError)](const Storage::Result& status) mutable {
        const auto stillCurrent=[&]{return tree_->GetTreeCtrl().GetSelectedItem()==anchor && (!mount||mounts_.IsCurrent(*mount));};
        if(!stillCurrent())return;
        result->status=status;
        if(status.error==ERROR_BUSY){
            const auto& key=result->listing.resourceKey.empty()?request.resource:result->listing.resourceKey;
            Defer(key,request.folder.IsClassic(),[this,anchor,mount,retry,restoreOnError]{
                if(tree_->GetTreeCtrl().GetSelectedItem()==anchor && (!mount||mounts_.IsCurrent(*mount))){
                    if(restoreOnError)listingPending_=false;
                    retry();
                }
            },true,true);
            return;
        }
        if(status.code){
            if(restoreOnError)listingPending_=false;
            if(nextOnError&&nextOnError())return;
            RejectNavigation(request.resource,status.message);
            return;
        }
        const auto& listing=result->listing;
        if(!StorageLocation::RevisionCurrent(listing.resourceKey,listing.revision)){
            if(restoreOnError)listingPending_=false;
            retry();
            return;
        }
        if(!request.reveal.IsNone() && std::none_of(listing.rows.begin(),listing.rows.end(),
            [&](const auto& row){return MatchesNavigationEntry(row,request.reveal);})){
            RejectNavigation(request.resource,"The search result changed or is hidden.");
            return;
        }
        if(restoreOnError&&!result->journalPrompt.empty())listingPending_=false;
        publish(*result);
        // A drive probe may hand off to another navigation (for example an HFS+ CD).
        if(Navigating())return;
        if(restoreOnError&&result->listing.folder.IsNone())listingPending_=false;
        if(request.folder.IsMounted() && result->listing.folder.Volume()!=request.folder.Volume())selection.clear();
        if(!request.reveal.IsNone())selection={request.reveal};
        RestoreListingSelection(selection,!request.reveal.IsNone());
#ifdef HFV_TESTING
        if(request.intent==BrowserNavigation::Intent::RefreshFolder)++notificationRefreshes;
#endif
    };
    listingPending_=true;
    notifications_->SetTimer(3,25,nullptr);
    if(status_)status_->SetPaneText(0,openingImage?"Opening image...":"Loading folder...");
}

bool BrowserSession::RevealFindHit(const FindHit& hit,int legacyVolume)
{
    using namespace BrowserNavigation;
    if(!tree_||!list_||!document_||hit.destination.IsNone())return false;
    Request request{hit.destination,Intent::RevealFindHit,{},hit.reveal,hit.nativePath};
    if(request.folder.IsMounted()){
        request.resource=request.folder.Volume()->ResourceKey();
        if(!hit.reveal.IsNone()&&!request.catalogRoute.empty())request.catalogRoute.pop_back();
        return browser_->RevealFindHit(request);
    }
    if(request.folder.IsClassic()){
        request.resource=request.folder.expectedResource;
        return document_->NavigateClassicFolder(legacyVolume,request.folder.expectedCatalogId,false,&request);
    }
    const auto& path=request.folder.WindowsPath();
    return document_->NavigateWindowsFolder(legacyVolume,path,false,&request);
}

void BrowserSession::NavigationMediaChanged(DWORD driveMask)
{
    mounts_.MediaChanged(driveMask);
    if(preview_)preview_->MediaChanged(driveMask);
    if(navigationMediaDrive_<0 || !(driveMask&(DWORD(1)<<navigationMediaDrive_)))return;
    CancelNavigation();
    RejectNavigation({},"The selected media changed during navigation.");
}

void BrowserSession::CompleteNavigation()
{
    if (!navigation_ || !navigation_->done() || StorageUi::busy() || StorageUi::state().sessionOpen)
        return;
    auto job = std::move(navigation_);
    auto ready = std::move(navigationReady_);
    notifications_->KillTimer(3);
    if (job->cancelled() || !tree_ || !list_)
        return;
    try {
        ready(job->result());
    } catch (const std::exception& error) {
        RejectNavigation({},error.what());
    }
    if(!navigation_&&!deferred_)navigationMediaDrive_=-1;
}

void BrowserSession::RejectNavigation(const std::string& key,const std::string& message)
{
    if (status_)
        status_->SetPaneText(0, message.c_str());
    if (current_.key == key) {
        current_.revision = VolumeCoord::Revision(key);
        dirty_.erase(key);
    }
#ifdef HFV_TESTING
    ++notificationErrors;
    navigationErrorForTest=message;
#endif
}

bool BrowserSession::Defer(const std::string& key,bool classic,std::function<void()> next,bool force,bool keepListing){
 if(!force&&!(classic&&key.empty()&&BackgroundCopies::Active())&&VolumeCoord::Readable(key))return false;
 if(!keepListing)ClearCurrent();
 deferredKey_=key;deferredClassic_=classic;deferred_=std::move(next);
 if(list_&&!keepListing){list_->list_clear();list_->SetBrowserCapabilities({});}
 if(status_)status_->SetPaneText(0,keepListing?"Showing the last folder listing while this volume is busy.":"Waiting for this volume to finish its file operation...");
 if(notifications_)notifications_->SetTimer(2,100,nullptr);return true;
}
BrowserSession::~BrowserSession(){DetachPanes();notifications_.reset();}
void BrowserSession::Attach(CWnd* owner,HFVExplorerTreeView* tree,CHFVExplorerListView* list,CHFVPreviewView* preview,CStatusBar* status){
    if(!owner||!tree||!list)throw std::logic_error("Browser session requires an owner and both browser panes");
    if((tree->BrowserBinding()&&tree->BrowserBinding()!=this)||(list->BrowserBinding()&&list->BrowserBinding()!=this)||(preview&&preview->BrowserBinding()&&preview->BrowserBinding()!=this))
        throw std::logic_error("A browser pane already belongs to another session");
    if((tree_&&tree_!=tree)||(list_&&list_!=list))throw std::logic_error("Detach the existing browser panes before rebinding a session");
    DetachPanes();if(!notifications_)notifications_.reset(new Notifications(*this));subscription_.Bind(notifications_->GetSafeHwnd());owner_=owner;tree_=tree;list_=list;preview_=preview;status_=status;
    tree_->BindBrowserSession(this);list_->BindBrowserSession(this);if(preview_)preview_->BindBrowserSession(this);SetLocationTitle({});
}
void BrowserSession::StopNotifications(){
 CancelNavigation();if(preview_)preview_->Clear();subscription_.Close();deferred_={};current_=Context{};seen_.clear();dirty_.clear();mounts_.ClearObserved();
 if(notifications_){notifications_->KillTimer(1);notifications_->KillTimer(2);}
}
void BrowserSession::DetachPanes(){
    ClearCurrent();subscription_.Close();current_=Context{};seen_.clear();dirty_.clear();mounts_.ClearObserved();
    browser_->ForgetTree();
    if(tree_)tree_->BindBrowserSession(nullptr);if(list_)list_->BindBrowserSession(nullptr);if(preview_)preview_->BindBrowserSession(nullptr);
    tree_=nullptr;list_=nullptr;preview_=nullptr;owner_=nullptr;status_=nullptr;
}
void BrowserSession::Detach(HFVExplorerTreeView* view){if(tree_==view){CancelNavigation();tree_=nullptr;browser_->ForgetTree();}if(!tree_&&!list_&&!preview_){subscription_.Close();current_=Context{};owner_=nullptr;status_=nullptr;}}
void BrowserSession::Detach(CHFVExplorerListView* view){if(list_==view){CancelNavigation();list_=nullptr;}if(!tree_&&!list_&&!preview_){subscription_.Close();current_=Context{};owner_=nullptr;status_=nullptr;}}
void BrowserSession::Detach(CHFVPreviewView* view){if(preview_==view)preview_=nullptr;if(!tree_&&!list_&&!preview_){subscription_.Close();current_=Context{};owner_=nullptr;status_=nullptr;}}
BrowserSession& CHFVExplorerListView::GetBrowserSession()const{
    if(m_browserSession)return *m_browserSession;
    if(m_pDocument)return static_cast<CHFVExplorerDoc*>(m_pDocument)->GetBrowserSession();
    throw std::logic_error("List view has no browser session");
}
BrowserSession& HFVExplorerTreeView::GetBrowserSession()const{
    if(m_browserSession)return *m_browserSession;
    if(m_pDocument)return static_cast<CHFVExplorerDoc*>(m_pDocument)->GetBrowserSession();
    throw std::logic_error("Tree view has no browser session");
}

BrowserSession& CMainFrame::GetBrowserSession()const{
    if(m_browserSession)return *m_browserSession;
    auto doc=static_cast<CHFVExplorerDoc*>(const_cast<CMainFrame*>(this)->GetActiveDocument());
    if(!doc)throw std::logic_error("Frame has no browser document");return doc->GetBrowserSession();
}

void BrowserSession::Watch(const std::string& key){subscription_.Watch(key);}
void BrowserSession::PrepareFolderPresentation(HTREEITEM item,const std::optional<std::wstring>& parent,bool mac)
{
 auto& control=tree_->GetTreeCtrl();
 const auto enabled=tree_->SelectionNotificationsEnabled();
 tree_->enable_selchange(0);
 if(item&&control.GetSelectedItem()!=item){
  control.SelectItem(item);
  control.EnsureVisible(item);
 }
 tree_->enable_selchange(enabled);
 list_->InitializeFolderRows(parent,mac);
}

void BrowserSession::PublishFolder(const BrowserDirectory::Snapshot& snapshot,HTREEITEM item,
    const std::optional<std::wstring>& parent,std::wstring title,const CString& status,
    const std::function<HTREEITEM(const BrowserDirectory::Row&)>& addFolder)
{
    const auto* node=tree_->BrowserNodeAt(item);
    if(!node||snapshot.folder.IsNone())throw std::logic_error("Folder result has no browser mount");
    const auto address=node->mount;
    BrowserPresentation::Batch treeBatch(tree_->GetTreeCtrl().m_hWnd);
    BrowserPresentation::Batch listBatch(list_->GetListCtrl().m_hWnd,true);
    PrepareFolderPresentation(item,parent,!snapshot.folder.IsWindows());
    if(snapshot.capabilitiesKnown)list_->SetBrowserCapabilities(snapshot.capabilities);
    for(const auto& row:snapshot.rows){
        const auto child=row.entry.directory?addFolder(row):nullptr;
        if(child)tree_->BindBrowserFolder(child,{row.location,row.capabilities.create});
        list_->InsertBrowserRow(row,child);
    }
    tree_->BindBrowserFolder(item,{snapshot.folder,snapshot.capabilities.create});
    if(snapshot.folder.IsMounted())list_->SortBrowserRows(FIELD_NAME,true);
    else list_->do_report_sort();
    Observe(snapshot.resourceKey,snapshot.revision,address,snapshot.folder);
    SetLocationTitle(std::move(title));
    if(status_)status_->SetPaneText(0,status);
    listingPending_=false;
    if(preview_)preview_->RequestSelection();
}

void BrowserSession::Observe(std::string key,uint64_t revision,BrowserMountInventory::Address address,FileOperations::Location folder)
{
 mounts_.Observe(address,key,folder.WindowsMediaSerial());
 current_={key,revision,address,std::move(folder)};
 Watch(key);seen_[key]=(std::max)(seen_[key],revision);
 if(dirty_[key]<=revision)dirty_.erase(key);
 // Close the subscribe-after-read race without acknowledging a newer write.
 auto latest=VolumeCoord::Revision(key);if(latest>revision){dirty_[key]=latest;if(notifications_)notifications_->PostMessage(VolumeCoord::Notification,WPARAM(subscription_.Id()),0);}
}
void BrowserSession::ProcessChanges(){
 std::set<std::string> changed;for(auto& event:subscription_.Drain())if(event.revision>seen_[event.key]){dirty_[event.key]=(std::max)(dirty_[event.key],event.revision);changed.insert(event.key);}
 if(!tree_||!list_||!IsWindow(tree_->GetSafeHwnd())||!IsWindow(list_->GetSafeHwnd()))return;
 // Inactive legacy roots are lazily refilled next time they are selected. Do
 // not leave old child paths pointing at renamed/deleted directories.
 if(document_){bool enabled=tree_->m_selchange_enabled!=0;tree_->enable_selchange(0);
  const auto& active=current_.mount;
  for(const auto& key:changed)for(const auto& root:mounts_.Observed(key)){
   if(root.address.kind==BrowserMountInventory::Kind::MountedVolume||
      (current_.key==key&&active&&root.address==*active)||!mounts_.IsCurrent(root))continue;
   const auto item=root.root;
   bool selected=false;for(auto at=tree_->GetTreeCtrl().GetSelectedItem();at;at=tree_->GetTreeCtrl().GetParentItem(at))if(at==item)selected=true;
   if(item&&!selected)tree_->delete_subdir_contents(item);
  }tree_->enable_selchange(enabled);
 }
 if(current_.key.empty()||dirty_[current_.key]<=current_.revision)return;
 StorageQuietRefresh quiet;auto context=current_;refreshing_=true;struct Done{bool& value;~Done(){value=false;}}done{refreshing_};
 const int column=list_->m_sorted_by_column;
 const bool ascending=list_->m_sort_is_ascending!=FALSE;
 const auto selection=CaptureListingSelection();
 try{
  if(!RefreshSelected(true))return;
  if(Navigating())return;
  list_->m_sorted_by_column=column;list_->m_sort_is_ascending=ascending;list_->SortBrowserRows(column,ascending);
  RestoreListingSelection(selection,false);
#ifdef HFV_TESTING
  ++notificationRefreshes;
#endif
 }catch(const std::exception& e){list_->list_clear();list_->SetBrowserCapabilities({});if(status_)status_->SetPaneText(0,e.what());current_=context;current_.revision=VolumeCoord::Revision(context.key);seen_[context.key]=current_.revision;dirty_.erase(context.key);
#ifdef HFV_TESTING
  ++notificationErrors;
#endif
 }
}
std::vector<FileOperations::Location> BrowserSession::CaptureListingSelection() const
{
 std::vector<FileOperations::Location> selection;
 auto& control=list_->GetListCtrl();
 for(int index=-1;(index=control.GetNextItem(index,LVNI_SELECTED))>=0;){
  const auto* row=list_->BrowserRowAt(index);
  if(!row||row->location.IsNone())continue;
  auto item=row->location;
  item.expectedIdentity.clear();
  item.expectedName.clear();
  item.expectedParent=0;
  selection.push_back(std::move(item));
 }
 return selection;
}

void BrowserSession::RestoreListingSelection(const std::vector<FileOperations::Location>& selection,bool reveal)
{
 if(selection.empty())return;
 auto& control=list_->GetListCtrl();
 bool restored=false;
 for(int index=0;index<control.GetItemCount();++index){
  const auto* row=list_->BrowserRowAt(index);
  if(!row||std::none_of(selection.begin(),selection.end(),[&](const auto& item){return MatchesNavigationEntry(*row,item);}))
   continue;
  if(!restored)control.SetItemState(-1,0,LVIS_SELECTED|LVIS_FOCUSED);
  const UINT state=LVIS_SELECTED|(!restored?LVIS_FOCUSED:0);
  control.SetItemState(index,state,state);
  if(!restored&&reveal)control.EnsureVisible(index,FALSE);
  restored=true;
 }
}

bool BrowserSession::RefreshSelected(bool afterMutation)
{
 if(!tree_)return false;
 const auto item=tree_->GetTreeCtrl().GetSelectedItem();
 const auto* binding=tree_->BrowserNodeAt(item);
 if(!binding)return false;
 const auto address=binding->mount;
 if(!mounts_.Lookup(address))return false;
 if(afterMutation&&(!current_.mount||!(*current_.mount==address)))return false;
 if(address.kind==BrowserMountInventory::Kind::MountedVolume)
  return afterMutation?browser_->RefreshForChange(current_.key):browser_->Refresh();
 if(!document_)return false;
 if(address.kind==BrowserMountInventory::Kind::WindowsDrive){
  const auto* folder=tree_->BrowserFolderAt(item);
  if(!folder||!folder->location.IsWindows())return false;
  const auto& path=folder->location.WindowsPath();
  if(afterMutation&&_wcsicmp(path.c_str(),current_.folder.WindowsPath().c_str()))return false;
  return document_->NavigateWindowsFolder(address.index,path,true);
 }
 const auto id=binding->CatalogId();
 if(afterMutation&&id!=current_.folder.expectedCatalogId)return false;
 return document_->NavigateClassicFolder(address.index,id,true);
}
