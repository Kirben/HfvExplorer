// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "BrowserTransfers.h"
#include "MainMacBrowser.h"
#include "VolumeCoordinator.h"
#include "DeviceWait.h"
#include <algorithm>
namespace BrowserTransfers {
namespace {
Selection clipboard;
uint64_t revision=0;
std::string Resource(const FileOperations::Location& p){return p.StorageKey();}
bool Same(const FileOperations::Location& a,const FileOperations::Location& b){
 if(a.LegacyType()!=b.LegacyType()||Resource(a)!=Resource(b))return false;
 if(a.Volume()||b.Volume())return a.Volume()&&b.Volume()&&a.Id()==b.Id();
 return a.LegacyType()==LIST_FAT?!_wcsicmp(a.WindowsPath().c_str(),b.WindowsPath().c_str()):!_stricmp(a.ClassicPath().c_str(),b.ClassicPath().c_str());
}
enum class DropKind { None, Folder, IconCanvas, Application };
struct Target {
 BrowserSession* session=nullptr;
 HWND control=nullptr;
 HTREEITEM item=nullptr;
 int row=-1;
 bool tree=false,writable=false;
 DropKind kind=DropKind::None;
 FileOperations::Location location;
};

Target Resolve(BrowserSession& session,bool tree,HTREEITEM item,int row,bool local){
 Target out;
 auto list=session.List();
 auto tv=session.Tree();
 if(!list||!tv)return out;
 out.session=&session;
 out.tree=tree;
 out.item=item;
 out.row=row;
 out.control=tree?tv->GetSafeHwnd():list->GetSafeHwnd();
 try{
  if(tree){
   if(!item)return {};
   auto folder=session.MacBrowser().DescribeFolder(item);
   out.location=std::move(folder.location);
   out.writable=folder.writable;
  }else{
   if(!session.ListingAvailable())return {};
   if(row<0){
    if(local){
     const auto style=list->GetStyle()&LVS_TYPEMASK;
     if(style!=LVS_ICON&&style!=LVS_SMALLICON)return {};
     out.kind=DropKind::IconCanvas;
     return out;
    }
    out.location=session.MacBrowser().TargetFolder();
    out.writable=list->BrowserDirectoryWritable();
   }else if(list->IsParentRow(row)){
    auto& control=tv->GetTreeCtrl();
    auto parent=control.GetParentItem(control.GetSelectedItem());
    if(!parent)return {};
    auto folder=session.MacBrowser().DescribeFolder(parent);
    out.location=std::move(folder.location);
    out.writable=folder.writable;
   }else{
    const auto model=list->BrowserRowAt(row);
    if(!model||model->location.IsNone())return {};
    out.location=model->location;
    if(!model->entry.directory){
     if(!local||!model->entry.readError.empty()||out.location.IsMounted())return {};
     const auto& finder=model->entry.finder;
     if(!out.location.IsWindows()||finder[0])return {};
     out.kind=DropKind::Application;
     return out;
    }
    out.writable=model->capabilities.create;
   }
  }
  out.kind=DropKind::Folder;
 }catch(const std::exception&){return {};}
 return out;
}

Target Hit(CPoint screen,HWND source){
 const auto window=WindowFromPoint(screen);
 const auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
 for(auto frame:app->m_browsers){
  auto& session=frame->GetBrowserSession();
  const bool local=frame->GetSafeHwnd()==source;
  auto tree=session.Tree();
  auto list=session.List();
  if(tree&&window==tree->GetSafeHwnd()){
   auto point=screen;
   tree->ScreenToClient(&point);
   UINT flags=0;
   auto item=tree->GetTreeCtrl().HitTest(point,&flags);
   if(item&&(flags&(TVHT_ONITEM|TVHT_ONITEMRIGHT)))return Resolve(session,true,item,-1,local);
   return {};
  }
  if(list&&window==list->GetSafeHwnd()){
   auto point=screen;
   list->ScreenToClient(&point);
   CRect bounds;
   list->GetClientRect(&bounds);
   if(bounds.PtInRect(point))return Resolve(session,false,nullptr,list->GetListCtrl().HitTest(point),local);
  }
 }
 return {};
}

Effect DropEffect(const Selection& selection,const Target& target,UINT keys)
{
 if(target.kind==DropKind::Folder)return Choose(selection,target.location,target.writable,keys);
 if(selection.items.empty()||StorageUi::busy()||StorageUi::state().sessionOpen)return Effect::None;
 if(target.kind==DropKind::IconCanvas)return Effect::Move;
 if(target.kind==DropKind::Application){
  const auto& source=selection.items.front();
  const auto& app=target.location;
  if(source.IsWindows()&&app.IsWindows()&&!_wcsicmp(source.WindowsPath().c_str(),app.WindowsPath().c_str()))
   return Effect::None;
  if(target.location.IsWindows()&&source.IsWindows())return Effect::Copy;
 }
 return Effect::None;
}

bool Transfer(const Selection& selection,const Target& target,UINT keys)
{
 const auto effect=DropEffect(selection,target,keys);
 if(effect==Effect::None)return false;
 if(target.kind==DropKind::Application)
  return target.session->List()->OpenWithDrop(selection.items.front(),target.location);
 if(target.kind!=DropKind::Folder)return false;
 auto sources=selection.items;
 for(auto& item:sources)item.cut=effect==Effect::Move;
 return target.session->MacBrowser().TransferSelection(sources,target.location);
}

}
Selection Snapshot(){return clipboard;}
void Publish(Selection value,bool append){
 if(value.items.empty())return;
 for(const auto& item:value.items)if(item.IsNone())throw HfsPlus::Error("Clipboard entries need a file location");
 bool movable=value.canMove&&(!append||clipboard.items.empty()||clipboard.canMove);
 if(!append)clipboard={};
 for(auto& item:value.items){auto found=std::find_if(clipboard.items.begin(),clipboard.items.end(),[&](const FileOperations::Location& p){return Same(item,p);});if(found==clipboard.items.end())clipboard.items.push_back(std::move(item));else *found=std::move(item);}
 clipboard.canMove=movable;++revision;
}
void Clear(){clipboard={};++revision;}
bool Any(){return !clipboard.items.empty();}
bool Paste(BrowserSession& session){
 if(!Any()||StorageUi::busy()||StorageUi::state().sessionOpen)return true;
 auto items=clipboard.items;auto token=revision;
 session.MacBrowser().TransferSelection(items,session.MacBrowser().TargetFolder(),[token](bool complete){
 if(complete&&revision==token){clipboard.items.erase(std::remove_if(clipboard.items.begin(),clipboard.items.end(),[](const FileOperations::Location& p){return p.cut;}),clipboard.items.end());++revision;}
 });
 return true;
}
Effect Choose(const Selection& selection,const FileOperations::Location& target,bool writable,UINT keys){
 if(!writable||target.IsNone()||selection.items.empty()||std::any_of(selection.items.begin(),selection.items.end(),[](const FileOperations::Location& item){return item.IsNone();})||StorageUi::busy()||StorageUi::state().sessionOpen)return Effect::None;
 if(keys&MK_CONTROL)return Effect::Copy;
 if(keys&MK_SHIFT)return selection.canMove?Effect::Move:Effect::None;
 if(!selection.canMove)return Effect::Copy;
 auto key=Resource(target);for(auto& item:selection.items)if(Resource(item)!=key)return Effect::Copy;
 return Effect::Move;
}
Drag::Drag(BrowserSession& session):selection_(session.MacBrowser().SelectedItems(false)),source_(session.Owner()->GetSafeHwnd()){}
Drag::~Drag(){ClearHighlight();}
void Drag::ClearHighlight(){if(highlight_&&IsWindow(highlight_)){if(treeHighlight_)SendMessage(highlight_,TVM_SELECTITEM,TVGN_DROPHILITE,0);else{LVITEM item{};item.stateMask=LVIS_DROPHILITED;SendMessage(highlight_,LVM_SETITEMSTATE,WPARAM(-1),reinterpret_cast<LPARAM>(&item));}}highlight_=nullptr;highlightItem_=nullptr;highlightRow_=-1;}
void Drag::Hover(CPoint screen,UINT keys)
{
 if(!IsWindow(source_)){ClearHighlight();return;}
 const auto target=Hit(screen,source_);
 const auto effect=DropEffect(selection_,target,keys);
 if(effect==Effect::None || target.kind==DropKind::IconCanvas)ClearHighlight();
 else if(highlight_!=target.control||treeHighlight_!=target.tree||highlightItem_!=target.item||highlightRow_!=target.row){
  ClearHighlight();
  highlight_=target.control;
  treeHighlight_=target.tree;
  highlightItem_=target.item;
  highlightRow_=target.row;
  if(target.tree)SendMessage(highlight_,TVM_SELECTITEM,TVGN_DROPHILITE,reinterpret_cast<LPARAM>(target.item));
  else if(target.row>=0){
   LVITEM item{};
   item.state=item.stateMask=LVIS_DROPHILITED;
   SendMessage(highlight_,LVM_SETITEMSTATE,target.row,reinterpret_cast<LPARAM>(&item));
  }
 }
 auto frame=DYNAMIC_DOWNCAST(CMainFrame,CWnd::FromHandlePermanent(source_));
 auto list=frame?frame->GetBrowserSession().List():nullptr;
 if(effect==Effect::None)SetCursor(LoadCursor(nullptr,IDC_NO));
 else if(list)list->set_cursor_by_type(effect==Effect::Copy);
}

bool Drag::Drop(CPoint screen,UINT keys)
{
 ClearHighlight();
 if(!IsWindow(source_))return true;
 const auto target=Hit(screen,source_);
 if(target.kind==DropKind::IconCanvas)
  return DropEffect(selection_,target,keys)==Effect::None;
 Transfer(selection_,target,keys);
 return true;
}

#ifdef HFV_TESTING
bool Drag::TestDrop(BrowserSession& session,bool tree,HTREEITEM item,int row,UINT keys)
{
 if(!IsWindow(source_))return false;
 return Transfer(selection_,Resolve(session,tree,item,row,session.Owner()->GetSafeHwnd()==source_),keys);
}
#endif
}
