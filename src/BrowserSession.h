// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <functional>
namespace BrowserDirectory { struct Row; struct Snapshot; }
#include "VolumeCoordinator.h"
namespace Storage { class Job; struct Result; }
class CHFVExplorerDoc;
class HFVExplorerTreeView;
class CHFVExplorerListView;
class CHFVPreviewView;
class MainMacBrowser;
// One session per browser document/window. Preferences and storage queues
// remain application-wide; navigation and pane bindings do not.
class BrowserSession {
 struct Notifications;
 std::unique_ptr<Notifications> notifications_;
 VolumeCoord::Subscription subscription_;
 struct Context {std::string key,path;uint64_t revision=0;int type=0,index=-1;uint32_t folder=2;};
 struct KnownRoot{int type,index;std::string origin;};
 std::map<std::string,std::vector<KnownRoot>> roots_;
 Context current_;
 std::map<std::string,uint64_t> seen_,dirty_;
 bool refreshing_=false,listingPending_=false;
 std::wstring locationTitle_;
 std::function<void()> deferred_;std::string deferredKey_;bool deferredClassic_=false;
 std::shared_ptr<Storage::Job> navigation_;
 std::function<void(const Storage::Result&)> navigationReady_;
 void CompleteNavigation();
 void ProcessChanges();
 void RefreshLegacy(const Context&);

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
    void StopNotifications();
    CHFVExplorerDoc* Document()const{return document_;}
    HFVExplorerTreeView* Tree()const{return tree_;}
    CHFVExplorerListView* List()const{return list_;}
    CHFVPreviewView* Preview()const{return preview_;}
    CWnd* Owner()const{return owner_;}
    CStatusBar* Status()const{return status_;}
    MainMacBrowser& MacBrowser()const{return *browser_;}
    void Watch(const std::string&);
    void PresentDirectory(const BrowserDirectory::Snapshot&, int type, int index,
        uint32_t folder, const std::string& path, bool listing,
        const std::function<HTREEITEM(const BrowserDirectory::Row&)>& addFolder);
    void Observe(std::string key,uint64_t revision,int type,int index,uint32_t folder=2,std::string path={});
    bool Refreshing()const{return refreshing_;}
    void ClearCurrent();
    void CancelNavigation();
    void Navigate(std::function<Storage::Result(Storage::Job&)>,
                  std::function<void(const Storage::Result&)>);
    bool UsesAsyncNavigation() const;
    bool Navigating() const { return bool(navigation_); }
    bool ListingAvailable() const { return !listingPending_; }
    void RejectNavigation(const std::string& key,const std::string& message);
    std::string ObservedWindowsKey(const std::string& path)const;
 void SetLocationTitle(std::wstring);
 std::wstring WindowTitle()const;
 void UpdateWindowTitle()const;
 bool Defer(const std::string& key,bool classic,std::function<void()>,bool force=false,bool keepListing=false);
#ifdef HFV_TESTING
    bool asyncNavigationForTest=false;
    unsigned notificationRefreshes=0,notificationErrors=0;
#endif
    bool finderLayout=false;
};
