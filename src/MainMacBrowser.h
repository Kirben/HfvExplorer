// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
class CHFVExplorerListView;
class MainMacBrowser {
    struct State;
    std::unique_ptr<State> state_;
    MainMacBrowser();
public:
    ~MainMacBrowser();
    static MainMacBrowser &Get();
    int OpenImage(const char *path);
    int OpenImage(const std::wstring &path);
    int OpenCd(const char *root);
    void DetachTree();
    void RebuildTree();
    void Clear();
    bool Select(LPARAM treeData);
    bool Active() const;
    bool Activate(int row);
    bool Refresh();
    bool NewFolder();
    bool Rename();
    bool Delete(bool confirm);
    bool Properties(int row);
    bool Capture(bool cut, bool append);
    bool Paste();
    void LegacyClipboardChanged();
    int Drop(int row, HTREEITEM target, int action);
    bool DropSelection(HTREEITEM target,int action);
    bool Command(UINT);
    bool ContextMenu(CPoint);
    bool Find();
    void Import();
    void ImportResource();
    void Export(unsigned fork);
    bool Writable() const;
    bool SelectionWritable() const;
    bool CanExport() const;
    int SelectedCdDrive() const;
    void ReleaseCd(int drive);
    void Sort(int column, bool ascending);
#ifdef HFV_TESTING
    int AttachTestVolume(std::shared_ptr<MacBrowser::Volume>, const char *key);
    bool ShowTestFolder(int index, uint32_t id);
#endif
};
