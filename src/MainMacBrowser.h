// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "DmgEncryption.h"
#include "BrowserTransfers.h"
class CHFVExplorerListView;
class BrowserSession;
namespace Removable { struct Mounted; }
namespace MediaDiscovery { struct DriveUpdate; }
namespace VolumeCoord { class Lease; }
namespace BrowserNavigation { struct Request; }
namespace BrowserDirectory { struct Row; struct Folder; }
class MainMacBrowser {
    BrowserSession& session_;
    void PublishDriveUpdate(MediaDiscovery::DriveUpdate&,std::shared_ptr<VolumeCoord::Lease>);
    struct State;
    std::unique_ptr<State> state_;
    Dmg::Credentials ImageCredentials(Dmg::UnlockMethod,bool retry,const std::wstring* apfsHint=nullptr,const std::wstring& target={});
    int QueueImageOpen(const std::wstring&,const std::wstring&,
        std::function<bool()> next={},Dmg::Credentials credentials={},
        std::string expectedImage={});
#ifdef HFV_TESTING
    std::function<std::shared_ptr<const Dmg::Password>(bool)> passwordPromptForTest_;
#endif
    bool AllowsFolderOperation(bool MacBrowser::Capabilities::* capability)const;
    bool QueueNextImage(const std::shared_ptr<const std::vector<std::wstring>>&,size_t);
public:
    ~MainMacBrowser();
    explicit MainMacBrowser(BrowserSession&);
    void ForgetTree();
#ifdef HFV_TESTING
    void SetPasswordPromptForTest(std::function<std::shared_ptr<const Dmg::Password>(bool)> prompt){
        passwordPromptForTest_=std::move(prompt);
    }
#endif
    int OpenImage(const char *path);
    void OpenImages(std::vector<std::wstring> paths);
    int OpenImage(const std::wstring &path,const std::wstring &externalJournal = {});
    bool HasCdRoot(int drive) const;
    void AdoptCd(int drive,const std::string& root,std::shared_ptr<MacBrowser::Volume>,bool select=true);
    void ScanStartupDrives(bool floppies,bool cds);
    void AdoptRemovable(std::vector<Removable::Mounted>&,int drive=-1,bool select=true);
    void QueueMediaChanges(DWORD changed,DWORD arrived,bool rescan=false);
    void ScanMedia();
    enum class MediaUpdate { Idle, Working, Retry };
    MediaUpdate ApplyMediaChanges();
    bool MediaUpdatesPending() const;
    void ReconcileRemovable(std::vector<Removable::Mounted>&,int drive,const std::vector<std::wstring>& failed={});
    void ScanRemovable(bool silent=false,bool startupOnly=false);
    void DetachTree();
    bool RebuildTree();
    void Clear();
    bool Select(LPARAM treeData);
    bool Active() const;
    std::string SelectedResourceKey()const;
    std::shared_ptr<MacBrowser::Volume> SelectedVolume() const;
    bool CanRestoreSnapshotSelection() const;
    bool CanRenameApfsVolume() const;
    bool RenameApfsVolume();
    bool RestoreSnapshotSelection();
    bool Activate(int row);
    bool Refresh();
    bool RefreshForChange(const std::string&);
    bool NewFolder();
    bool CreateFolder(const FileOperations::Location&,const std::u16string&);
    bool Rename();
    bool RenameItem(int row,const std::u16string& name,bool checkLock=true);
    bool RenameWindowsItem(int row);
    FileOperations::Location CaptureItem(int row);
    bool Delete(bool confirm);
    bool CanInspectProperties(int row) const;
    bool Properties(int row);
    bool Capture(bool cut, bool append);
    bool Paste();
    bool CanPasteHardLink()const;
    bool PasteHardLink();
    BrowserTransfers::Selection SelectedItems(bool cut);
    FileOperations::Location TargetFolder(HTREEITEM item=nullptr);
    BrowserDirectory::Folder DescribeFolder(HTREEITEM item=nullptr);
    bool TransferSelection(const std::vector<FileOperations::Location>&,const FileOperations::Location&,std::function<void(bool)> completed={});
    bool Command(UINT);
    bool ContextMenu(CPoint);
    bool OpenMountedFolderFrom(const MainMacBrowser& source,const BrowserNavigation::Request& request);
    bool RevealFindHit(const BrowserNavigation::Request&);
    void Import();
    void ImportResource();
    void Export(unsigned fork);
    bool Writable() const;
    bool CanCreateFolder()const;
    bool CanRename()const;
    bool CanDelete()const;
    bool SelectionWritable() const;
    bool CanImportResource()const;
    bool CanExport() const;
    int SelectedCdDrive() const;
    void ReleaseCd(int drive);
    std::vector<std::shared_ptr<MacBrowser::Volume>> DetachCd(int drive);
    void Sort(int column, bool ascending);
#ifdef HFV_TESTING
    void SetDriveTypeForTest(std::function<UINT(int)>);
    size_t TestClipboardCount()const;
    int AttachTestVolume(std::shared_ptr<MacBrowser::Volume>, const char *key,int drive=-1);
    bool ShowTestFolder(int index, uint32_t id);
#endif
};
