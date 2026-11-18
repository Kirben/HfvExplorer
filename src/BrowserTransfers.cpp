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
struct Target {
 BrowserSession* session=nullptr;HWND control=nullptr;HTREEITEM item=nullptr;int row=-1;bool tree=false,writable=false;
 FileOperations::Location folder;
};
Target Resolve(BrowserSession& session,bool tree,HTREEITEM item,int row){
 Target out;out.session=&session;out.tree=tree;out.item=item;out.row=row;
 auto list=session.List();auto tv=session.Tree();if(!list||!tv)return {};
 out.control=tree?tv->GetSafeHwnd():list->GetSafeHwnd();
 if(!tree&&row>=0){auto p=list->BrowserRowAt(row);if(!p||!p->entry.directory)return {};out.writable=p->capabilities.create;auto binding=reinterpret_cast<list_lparam_struct*>(list->GetListCtrl().GetItemData(row));item=binding?binding->htree:nullptr;if(!item)return {};}
 else if(!tree)out.writable=list->BrowserDirectoryWritable();
 try{out.folder=session.MacBrowser().TargetFolder(item);if(tree){auto binding=reinterpret_cast<lparam_struct*>(tv->m_ct->GetItemData(item));if(!binding)return {};out.writable=out.folder.IsMounted()?out.folder.Volume()->Supports().create:out.folder.IsWindows()?session.Document()->m_fats[binding->volinx].m_type!=DRIVE_CDROM:!session.Document()->m_volumes[binding->volinx].m_is_cd;}}
 catch(const std::exception&){return {};}
 return out;
}
Target Hit(CPoint screen){
 HWND window=WindowFromPoint(screen);auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
 for(auto frame:app->m_browsers){auto& session=frame->GetBrowserSession();auto tree=session.Tree();auto list=session.List();
  if(tree&&window==tree->GetSafeHwnd()){CPoint p=screen;tree->ScreenToClient(&p);UINT flags=0;auto item=tree->GetTreeCtrl().HitTest(p,&flags);if(item&&(flags&(TVHT_ONITEM|TVHT_ONITEMRIGHT)))return Resolve(session,true,item,-1);return {};}
  if(list&&window==list->GetSafeHwnd()){CPoint p=screen;list->ScreenToClient(&p);CRect bounds;list->GetClientRect(&bounds);if(bounds.PtInRect(p))return Resolve(session,false,nullptr,list->GetListCtrl().HitTest(p));}
 }
 return {};
}
bool Transfer(const Selection& selection,Target& target,UINT keys){
 auto effect=Choose(selection,target.folder,target.session&&target.writable,keys);if(effect==Effect::None)return false;
 auto sources=selection.items;for(auto& item:sources)item.cut=effect==Effect::Move;
 return target.session->MacBrowser().TransferSelection(sources,target.folder);
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
bool Drag::Hover(CPoint screen,UINT keys){
 if(!IsWindow(source_)){ClearHighlight();return true;}HWND window=WindowFromPoint(screen);if(window==source_||IsChild(source_,window)){ClearHighlight();return false;}
 auto target=Hit(screen);auto effect=Choose(selection_,target.folder,target.session&&target.writable,keys);
 if(effect==Effect::None)ClearHighlight();
 else if(highlight_!=target.control||treeHighlight_!=target.tree||highlightItem_!=target.item||highlightRow_!=target.row){ClearHighlight();highlight_=target.control;treeHighlight_=target.tree;highlightItem_=target.item;highlightRow_=target.row;if(target.tree)SendMessage(highlight_,TVM_SELECTITEM,TVGN_DROPHILITE,reinterpret_cast<LPARAM>(target.item));else if(target.row>=0){LVITEM item{};item.state=item.stateMask=LVIS_DROPHILITED;SendMessage(highlight_,LVM_SETITEMSTATE,target.row,reinterpret_cast<LPARAM>(&item));}}
 auto frame=DYNAMIC_DOWNCAST(CMainFrame,CWnd::FromHandlePermanent(source_));
 auto list=frame?frame->GetBrowserSession().List():nullptr;
 if(effect==Effect::None)SetCursor(LoadCursor(nullptr,IDC_NO));else if(list)list->set_cursor_by_type(effect==Effect::Copy);
 return true;
}
bool Drag::Drop(CPoint screen,UINT keys){ClearHighlight();if(!IsWindow(source_))return true;HWND window=WindowFromPoint(screen);if(window==source_||IsChild(source_,window))return false;auto target=Hit(screen);Transfer(selection_,target,keys);return true;}
#ifdef HFV_TESTING
bool Drag::TestDrop(BrowserSession& session,bool tree,HTREEITEM item,int row,UINT keys){if(!IsWindow(source_))return false;auto target=Resolve(session,tree,item,row);return Transfer(selection_,target,keys);}
#endif
}
