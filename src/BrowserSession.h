// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
class CHFVExplorerDoc;
class HFVExplorerTreeView;
class CHFVExplorerListView;
class CHFVPreviewView;
class MainMacBrowser;
// One session per browser document/window. Preferences and the serialized
// storage worker remain application-wide; navigation and pane bindings do not.
class BrowserSession {
    CHFVExplorerDoc* document_;
    std::unique_ptr<MainMacBrowser> browser_;
    HFVExplorerTreeView* tree_=nullptr;
    CHFVExplorerListView* list_=nullptr;
    CHFVPreviewView* preview_=nullptr;
    CWnd* owner_=nullptr;
    CStatusBar* status_=nullptr;
public:
    explicit BrowserSession(CHFVExplorerDoc* document=nullptr);
    ~BrowserSession();
    BrowserSession(const BrowserSession&)=delete;
    BrowserSession& operator=(const BrowserSession&)=delete;
    void Attach(CWnd* owner,HFVExplorerTreeView* tree,CHFVExplorerListView* list,CHFVPreviewView* preview=nullptr,CStatusBar* status=nullptr);
    void Detach(HFVExplorerTreeView*);
    void Detach(CHFVExplorerListView*);
    void Detach(CHFVPreviewView*);
    void DetachPanes();
    CHFVExplorerDoc* Document()const{return document_;}
    HFVExplorerTreeView* Tree()const{return tree_;}
    CHFVExplorerListView* List()const{return list_;}
    CHFVPreviewView* Preview()const{return preview_;}
    CWnd* Owner()const{return owner_;}
    CStatusBar* Status()const{return status_;}
    MainMacBrowser& MacBrowser()const{return *browser_;}
    bool finderLayout=false;
};
