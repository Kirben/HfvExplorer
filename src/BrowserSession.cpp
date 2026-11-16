// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include <stdexcept>
BrowserSession::BrowserSession(CHFVExplorerDoc* doc):document_(doc),browser_(new MainMacBrowser(*this)){
    finderLayout=static_cast<CHFVExplorerApp*>(AfxGetApp())->m_mac_window_mode!=FALSE;
}
BrowserSession::~BrowserSession(){DetachPanes();}
void BrowserSession::Attach(CWnd* owner,HFVExplorerTreeView* tree,CHFVExplorerListView* list,CHFVPreviewView* preview,CStatusBar* status){
    if(!owner||!tree||!list)throw std::logic_error("Browser session requires an owner and both browser panes");
    if((tree->BrowserBinding()&&tree->BrowserBinding()!=this)||(list->BrowserBinding()&&list->BrowserBinding()!=this)||(preview&&preview->BrowserBinding()&&preview->BrowserBinding()!=this))
        throw std::logic_error("A browser pane already belongs to another session");
    if((tree_&&tree_!=tree)||(list_&&list_!=list))throw std::logic_error("Detach the existing browser panes before rebinding a session");
    DetachPanes();owner_=owner;tree_=tree;list_=list;preview_=preview;status_=status;
    tree_->BindBrowserSession(this);list_->BindBrowserSession(this);if(preview_)preview_->BindBrowserSession(this);
}
void BrowserSession::DetachPanes(){
    browser_->ForgetTree();
    if(tree_)tree_->BindBrowserSession(nullptr);if(list_)list_->BindBrowserSession(nullptr);if(preview_)preview_->BindBrowserSession(nullptr);
    tree_=nullptr;list_=nullptr;preview_=nullptr;owner_=nullptr;status_=nullptr;
}
void BrowserSession::Detach(HFVExplorerTreeView* view){if(tree_==view){tree_=nullptr;browser_->ForgetTree();}if(!tree_&&!list_&&!preview_){owner_=nullptr;status_=nullptr;}}
void BrowserSession::Detach(CHFVExplorerListView* view){if(list_==view)list_=nullptr;if(!tree_&&!list_&&!preview_){owner_=nullptr;status_=nullptr;}}
void BrowserSession::Detach(CHFVPreviewView* view){if(preview_==view)preview_=nullptr;if(!tree_&&!list_&&!preview_){owner_=nullptr;status_=nullptr;}}
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
