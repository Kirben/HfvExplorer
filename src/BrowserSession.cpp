// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "BackgroundCopies.h"
#include "DeviceWait.h"
#include "StorageJobDialog.h"
#include "BrowserPresentation.h"
#include "charset.h"
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
  if(message==WM_TIMER&&w==2){
   if(!session.owner_||!session.tree_||!session.list_){session.deferred_={};KillTimer(2);return 0;}
   if(StorageUi::busy()||StorageUi::state().sessionOpen||(session.deferredClassic_&&session.deferredKey_.empty()&&BackgroundCopies::Active())||!VolumeCoord::Readable(session.deferredKey_))return 0;
   KillTimer(2);auto next=std::move(session.deferred_);session.deferred_={};if(next)next();return 0;
  }
  if((message==VolumeCoord::Notification&&w==session.subscription_.Id())||(message==WM_TIMER&&w==1)){
   auto list=session.List();bool editing=list&&IsWindow(list->GetSafeHwnd())&&(list->GetListCtrl().GetEditControl()||list->IsDragging());
   if(StorageUi::busy()||StorageUi::state().sessionOpen||session.refreshing_||editing||(!session.current_.key.empty()&&!VolumeCoord::Available({{session.current_.key,session.current_.type==LIST_FAT?VolumeCoord::Mode::Browse:VolumeCoord::Mode::Read}}))){SetTimer(1,75,nullptr);return 0;}
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
std::string BrowserSession::ObservedWindowsKey(const std::string& path)const{return current_.type==LIST_FAT&&!_stricmp(current_.path.c_str(),path.c_str())?current_.key:std::string();}
void BrowserSession::ClearCurrent(){current_=Context{};deferred_={};if(notifications_)notifications_->KillTimer(2);}
bool BrowserSession::Defer(const std::string& key,bool classic,std::function<void()> next,bool force){
 if(!force&&!(classic&&key.empty()&&BackgroundCopies::Active())&&VolumeCoord::Readable(key))return false;
 ClearCurrent();deferredKey_=key;deferredClassic_=classic;deferred_=std::move(next);
 if(list_){list_->list_clear();list_->SetBrowserCapabilities({});}
 if(status_)status_->SetPaneText(0,"Waiting for this volume to finish its file operation...");
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
 subscription_.Close();deferred_={};current_=Context{};seen_.clear();dirty_.clear();roots_.clear();
 if(notifications_){notifications_->KillTimer(1);notifications_->KillTimer(2);}
}
void BrowserSession::DetachPanes(){
    ClearCurrent();subscription_.Close();current_=Context{};seen_.clear();dirty_.clear();roots_.clear();
    browser_->ForgetTree();
    if(tree_)tree_->BindBrowserSession(nullptr);if(list_)list_->BindBrowserSession(nullptr);if(preview_)preview_->BindBrowserSession(nullptr);
    tree_=nullptr;list_=nullptr;preview_=nullptr;owner_=nullptr;status_=nullptr;
}
void BrowserSession::Detach(HFVExplorerTreeView* view){if(tree_==view){tree_=nullptr;browser_->ForgetTree();}if(!tree_&&!list_&&!preview_){subscription_.Close();current_=Context{};owner_=nullptr;status_=nullptr;}}
void BrowserSession::Detach(CHFVExplorerListView* view){if(list_==view)list_=nullptr;if(!tree_&&!list_&&!preview_){subscription_.Close();current_=Context{};owner_=nullptr;status_=nullptr;}}
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
BrowserSession& CHFVPreviewView::GetBrowserSession()const{
    if(m_browserSession)return *m_browserSession;
    if(m_pDocument)return static_cast<CHFVExplorerDoc*>(m_pDocument)->GetBrowserSession();
    throw std::logic_error("Preview has no browser session");
}
BrowserSession& CMainFrame::GetBrowserSession()const{
    if(m_browserSession)return *m_browserSession;
    auto doc=static_cast<CHFVExplorerDoc*>(const_cast<CMainFrame*>(this)->GetActiveDocument());
    if(!doc)throw std::logic_error("Frame has no browser document");return doc->GetBrowserSession();
}

void BrowserSession::Watch(const std::string& key){subscription_.Watch(key);}
void BrowserSession::Observe(std::string key,uint64_t revision,int type,int index,uint32_t folder,std::string path){
 if(document_&&index>=0&&((type==LIST_FAT&&index<document_->m_fat_count)||(type==LIST_HFS&&index<document_->m_hfs_count))){
  auto& known=roots_[key];auto found=std::find_if(known.begin(),known.end(),[&](const KnownRoot& r){return r.type==type&&r.index==index;});
  std::string origin=type==LIST_FAT?(LPCSTR)document_->m_fats[index].m_volume_name:(LPCSTR)document_->m_volumes[index].m_file_name;
  if(found==known.end())known.push_back({type,index,origin});else found->origin=origin;
 }
 current_={key,std::move(path),revision,type,index,folder};
 if(type==LIST_FAT||type==LIST_HFS){auto title=VolumeCoord::Wide(current_.path);if(type==LIST_HFS&&!title.empty()&&title.back()==L':')title.pop_back();SetLocationTitle(std::move(title));}
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
  for(auto& key:changed)for(auto& root:roots_[key]){
   if(current_.key==key&&current_.type==root.type&&current_.index==root.index)continue;
   HTREEITEM item=nullptr;
   if(root.type==LIST_FAT&&root.index<document_->m_fat_count&&!_stricmp(root.origin.c_str(),document_->m_fats[root.index].m_volume_name))item=document_->m_fats[root.index].m_rootitem;
   if(root.type==LIST_HFS&&root.index<document_->m_hfs_count&&!_stricmp(root.origin.c_str(),document_->m_volumes[root.index].m_file_name))item=document_->m_volumes[root.index].m_rootitem;
   bool selected=false;for(auto at=tree_->GetTreeCtrl().GetSelectedItem();at;at=tree_->GetTreeCtrl().GetParentItem(at))if(at==item)selected=true;
   if(item&&!selected)tree_->delete_subdir_contents(item);
  }tree_->enable_selchange(enabled);
 }
 if(current_.key.empty()||dirty_[current_.key]<=current_.revision)return;
 StorageQuietRefresh quiet;auto context=current_;refreshing_=true;struct Done{bool& value;~Done(){value=false;}}done{refreshing_};
 int column=list_->m_sorted_by_column;bool ascending=list_->m_sort_is_ascending!=FALSE;std::vector<uint32_t> ids;std::vector<std::string> paths;
 auto& control=list_->GetListCtrl();for(int row=-1;(row=control.GetNextItem(row,LVNI_SELECTED))>=0;){auto entry=list_->BrowserRowAt(row);if(entry){ids.push_back(entry->id);paths.push_back(entry->path);}}
 try{
  if(context.type==LIST_MAC){if(!browser_->RefreshForChange(context.key))throw std::runtime_error("The selected volume could not be refreshed");}else RefreshLegacy(context);
  list_->m_sorted_by_column=column;list_->m_sort_is_ascending=ascending;list_->SortBrowserRows(column,ascending);
  bool restored=false;for(int row=0;row<control.GetItemCount();++row){auto entry=list_->BrowserRowAt(row);if(!entry)continue;bool selected=context.type==LIST_FAT?std::find(paths.begin(),paths.end(),entry->path)!=paths.end():std::find(ids.begin(),ids.end(),entry->id)!=ids.end();if(selected){if(!restored)control.SetItemState(-1,0,LVIS_SELECTED);control.SetItemState(row,LVIS_SELECTED,LVIS_SELECTED);restored=true;}}
#ifdef HFV_TESTING
  ++notificationRefreshes;
#endif
 }catch(const std::exception& e){list_->list_clear();list_->SetBrowserCapabilities({});if(status_)status_->SetPaneText(0,e.what());current_=context;current_.revision=VolumeCoord::Revision(context.key);seen_[context.key]=current_.revision;dirty_.erase(context.key);
#ifdef HFV_TESTING
  ++notificationErrors;
#endif
 }
}
void BrowserSession::RefreshLegacy(const Context& context){
 if(!document_)return;
 if(context.index<0||(context.type==LIST_FAT?context.index>=document_->m_fat_count:context.index>=document_->m_hfs_count))throw std::runtime_error("The volume is no longer attached");
 if(context.type==LIST_FAT){
  std::string path=context.path;auto result=ShowStorageWaitJob([&](Storage::Job&){VolumeCoord::Lease read({{context.key,VolumeCoord::Mode::Browse}});for(;;){DWORD a=GetFileAttributesA(path.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))break;DWORD error=GetLastError();if(error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("The Windows folder is unavailable");auto slash=path.find_last_of('\\');if(slash==2&&path[1]==':'){path.resize(3);continue;}if(slash==path.npos||slash<3)throw std::runtime_error("The Windows folder no longer exists");path.resize(slash);}return Storage::Result{};},!owner_->IsWindowVisible(),owner_);
  if(result.code)throw std::runtime_error(result.message);
  auto item=tree_->tree_find_fat(document_->m_fats[context.index].m_rootitem,const_cast<char*>(path.c_str()));if(!item)item=document_->m_fats[context.index].m_rootitem;
  bool enabled=tree_->m_selchange_enabled!=0;tree_->enable_selchange(0);tree_->GetTreeCtrl().SelectItem(item);tree_->delete_subdir_contents(item);tree_->enable_selchange(enabled);
  document_->open_fat_directory(context.index,const_cast<char*>(path.c_str()),1);return;
 }
 if(context.type!=LIST_HFS)return;auto& mounted=document_->m_volumes[context.index];std::string path;
 auto result=ShowStorageWaitJob([&](Storage::Job&){VolumeCoord::Lease read({{context.key,VolumeCoord::Mode::Read}});mounted.close();if(!mounted.open())throw std::runtime_error("The HFS volume could not be reopened");
  auto volume=hfs_mount(const_cast<char*>((LPCSTR)mounted.m_file_name),1,O_RDONLY);if(!volume)throw std::runtime_error(hfs_error?hfs_error:"The HFS volume is unavailable");struct Close{hfsvol* v;~Close(){hfs_umount(v);}}close{volume};
  long id=long(context.folder);if(hfs_setcwd(volume,id)<0)id=2;std::vector<std::string> names;std::set<long> visited;
  while(id!=2){if(!visited.insert(id).second)throw std::runtime_error("Cyclic HFS folder path");char name[32]{};if(hfs_dirinfo(volume,&id,name)<0){names.clear();break;}mac_to_pc_charset(reinterpret_cast<unsigned char*>(name));names.push_back(name);}
  document_->m_classicRevision[context.index]=VolumeCoord::Revision(context.key);path=(LPCSTR)mounted.m_volume_name;path+=':';for(auto at=names.rbegin();at!=names.rend();++at){path+=*at;path+=':';}return Storage::Result{};
 },!owner_->IsWindowVisible(),owner_);if(result.code)throw std::runtime_error(result.message);
 bool enabled=tree_->m_selchange_enabled!=0;tree_->enable_selchange(0);tree_->GetTreeCtrl().SelectItem(mounted.m_rootitem);tree_->delete_subdir_contents(mounted.m_rootitem);tree_->enable_selchange(enabled);
 document_->open_sub_directory(context.index,2,mounted.m_volume_name);document_->my_chdir(path.c_str(),1,context.index);
}
