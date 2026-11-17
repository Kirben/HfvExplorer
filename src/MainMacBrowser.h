// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "BrowserTransfers.h"
class CHFVExplorerListView;
class BrowserSession;
class MainMacBrowser {
    BrowserSession& session_;
    struct State;
    std::unique_ptr<State> state_;
public:
    ~MainMacBrowser();
    explicit MainMacBrowser(BrowserSession&);
    void ForgetTree();
    int OpenImage(const char *path);
    int OpenImage(const std::wstring &path,const std::wstring &externalJournal = {});
    int OpenCd(const char *root);
    void DetachTree();
    void RebuildTree();
    void Clear();
    bool Select(LPARAM treeData);
    bool Active() const;
    std::string SelectedResourceKey()const;
    bool Activate(int row);
    bool Refresh();
    bool RefreshForChange(const std::string&);
    bool NewFolder();
    bool CreateFolder(const FileOperations::Location&,const std::u16string&);
    bool Rename();
    bool RenameItem(int row,const std::u16string& name);
    FileOperations::Location CaptureItem(int row);
    bool Delete(bool confirm);
    bool Properties(int row);
    bool Capture(bool cut, bool append);
    bool Paste();
    BrowserTransfers::Selection SelectedItems(bool cut);
    FileOperations::Location TargetFolder(HTREEITEM item=nullptr);
    bool TransferSelection(const std::vector<FileOperations::Location>&,const FileOperations::Location&,std::function<void(bool)> completed={});
    int Drop(int row, HTREEITEM target, int action);
    bool DropSelection(HTREEITEM target,int action);
    bool Command(UINT);
    bool ContextMenu(CPoint);
    bool Find();
    void Import();
    void ImportResource();
    void Export(unsigned fork);
    bool Writable() const;
    bool CanCreateFolder()const;
    bool SelectionWritable() const;
    bool CanExport() const;
    int SelectedCdDrive() const;
    void ReleaseCd(int drive);
    void Sort(int column, bool ascending);
#ifdef HFV_TESTING
    size_t TestClipboardCount()const;
    int AttachTestVolume(std::shared_ptr<MacBrowser::Volume>, const char *key,int drive=-1);
    bool ShowTestFolder(int index, uint32_t id);
#endif
};
