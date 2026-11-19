// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LegacyEngine.h"
#include "BrowserSession.h"
#include "StorageLocation.h"
#include "MainMacBrowser.h"
#include "PhysicalRemovable.h"
#include "MediaDiscovery.h"
#include "Floppy.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "BrowserPresentation.h"
#include "WindowsFolderEntries.h"
#include "HfsPlusDisk.h"
#include "charset.h"
#include "aspecial.h"
#include "utils.h"
#include <stdexcept>
extern HICON get_dos_icon(CHFVExplorerApp*,char*,int);
extern int is_afp_directory(char*);
namespace {
// The old tree and icon adapters still take ANSI paths. Keep their path
// separate from the Unicode location used for selection and file operations.
std::string LegacyPath(const std::wstring& path) {
 const UINT page=GetACP();
 const bool utf8=page==CP_UTF8;
 const DWORD flags=utf8?0:WC_NO_BEST_FIT_CHARS;
 BOOL lost=FALSE;
 const auto count=WideCharToMultiByte(page,flags,path.c_str(),-1,nullptr,0,nullptr,utf8?nullptr:&lost);
 if(lost||count<=1||count>MAX_PATH)return {};
 std::string text(count,'\0');
 if(!WideCharToMultiByte(page,flags,path.c_str(),-1,text.data(),count,nullptr,utf8?nullptr:&lost)||lost)return {};
 text.pop_back();
 return text;
}

bool IsCompanion(const std::wstring& directory,const std::wstring& leaf) {
 if(leaf.size()<2||leaf.front()!=L'%')return false;
 if(GetFileAttributesW((directory+leaf.substr(1)).c_str())==INVALID_FILE_ATTRIBUTES)return false;
 try{return MacBrowser::WindowsResourceCompanion(directory+leaf);}
 catch(const std::exception&){return false;}
}
}

BrowserDirectory::Snapshot BrowserFolderReader::read_windows_browser_directory(
    int volume,const std::wstring& directory,bool listing,Storage::Job& job,std::optional<DWORD> expectedSerial)
{
 if(directory.empty())throw std::runtime_error("Select a Windows folder");
 BrowserDirectory::ObserveRead(BrowserPresentation::Utf8(MacBrowser::U16(directory)).c_str());
 BrowserDirectory::Snapshot snapshot;
 snapshot.folderPath=directory;
 snapshot.resourceKey=VolumeCoord::PathKey(snapshot.folderPath);
 // Browsing the folder can overlap a write to a child file. Both idle and
 // busy listings take the same lease and tolerate briefly locked children.
 const VolumeCoord::Lease access({{snapshot.resourceKey,VolumeCoord::Mode::Browse}});
 snapshot.revision=VolumeCoord::Revision(snapshot.resourceKey);
 wchar_t root[MAX_PATH]{};
 const auto driveType=GetVolumePathNameW(snapshot.folderPath.c_str(),root,ARRAYSIZE(root))?
     GetDriveTypeW(root):UINT(DRIVE_UNKNOWN);
 const bool writable=driveType!=DRIVE_CDROM;
 auto media=FileOperations::Location::Windows(snapshot.folderPath);
 if(expectedSerial) {
  media.BindWindowsMediaSerial(expectedSerial);
  StorageLocation::ValidateMedia(media);
 }else if(driveType==DRIVE_REMOVABLE||driveType==DRIVE_CDROM)
  media.BindWindowsMediaSerial(StorageLocation::WindowsSerial(snapshot.folderPath));
 snapshot.windowsMediaSerial=media.WindowsMediaSerial();
 try{snapshot.folderIdentity=MacBrowser::DescribeWindowsFile(snapshot.folderPath).identity;}
 catch(const std::exception&){}
 snapshot.capabilitiesKnown=true;
 snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=writable;

 auto folder=snapshot.folderPath;
 if(folder.back()!=L'\\'&&folder.back()!=L'/')folder+=L'\\';
 auto ioFolder=BrowserPresentation::WindowsPath(folder);
 if(ioFolder.back()!=L'\\')ioFolder+=L'\\';
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 std::string ansiFolder=LegacyPath(folder);
 const bool afp=listing&&!ansiFolder.empty()&&ansiFolder.size()+sizeof(":AFP_AfpInfo")<=MAX_PATH&&
     is_afp_directory(ansiFolder.data())!=0;
 const DWORD deadline=listing&&options_.windowsIcons!=DI_NO_ICONS&&options_.windowsIconTimeout>0?
     GetTickCount()+DWORD(options_.windowsIconTimeout)*1000:0;
 LegacyEngine::Scope engine;
 WindowsFolderEntries find(ioFolder+L"*");
 if(!find){
  if(find.Error()==ERROR_FILE_NOT_FOUND){
   StorageLocation::ValidateMedia(media);
   return snapshot;
  }
  throw std::runtime_error("Cannot enumerate Windows folder");
 }
 do{
  if(job.cancelled())throw Storage::Cancelled();
  const auto& info=find.Entry();
  const std::wstring leaf=info.cFileName;
  if(leaf==L"."||leaf==L".."||
     (!options_.showWindowsHidden&&(info.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN))||
     IsCompanion(ioFolder,leaf))continue;

  BrowserDirectory::Row row;
  row.storageType=LIST_FAT;
  row.volume=volume;
  row.capabilities=snapshot.capabilities;
  const auto path=folder+leaf;
  row.path=LegacyPath(path);
  if(row.path.empty()&&info.cAlternateFileName[0])row.path=LegacyPath(folder+info.cAlternateFileName);
  row.location=FileOperations::Location::Windows(path);
  row.location.BindWindowsMediaSerial(snapshot.windowsMediaSerial);
  row.entry.name=MacBrowser::U16(leaf);
  row.entry.directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  if(!row.entry.directory)
   row.entry.forks[0].size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
  row.created=BrowserDirectory::Ticks(info.ftCreationTime);
  row.modified=BrowserDirectory::Ticks(info.ftLastWriteTime);
  try{row.entry=MacBrowser::DescribeWindowsFile(path);}
  catch(const std::exception&){/* Keep the enumerated metadata if a child is briefly locked. */}

  const auto finderFlags=HfsPlus::Disk::U16(row.entry.finder.data()+8);
  if(!options_.showMacHidden&&(finderFlags&fInvisible))continue;
  if(listing&&!row.entry.directory&&!row.path.empty()) {
   auto ansiLeaf=LegacyPath(leaf);
   const bool hasFinder=std::any_of(row.entry.finder.begin(),row.entry.finder.end(),[](uint8_t byte){return byte!=0;});
   if(!ansiFolder.empty()&&!ansiLeaf.empty()&&ansiFolder.size()+ansiLeaf.size()+1<MAX_PATH) {
    char name[MAX_PATH],type[20]{},creator[20]{};
    int show=1;
    if(hasFinder){
     // The common AppleDouble parser has already supplied the metadata.
     // Use the legacy helper here only for its icon cache and artwork.
     get_apple_double(ansiFolder.c_str(),ansiLeaf.data(),&row.icon16,&row.icon32,name,type,creator,&show);
    }else if(afp) {
     get_afp(ansiFolder.c_str(),ansiLeaf.data(),&row.icon16,&row.icon32,name,type,creator,&show);
     memcpy(row.entry.finder.data(),type,(std::min)(strlen(type),size_t(4)));
     memcpy(row.entry.finder.data()+4,creator,(std::min)(strlen(creator),size_t(4)));
    }
   }
   if(!row.icon16&&!row.icon32) {
    const int mode=deadline&&LONG(GetTickCount()-deadline)>=0?DI_NO_ICONS:options_.windowsIcons;
    const auto icon=get_dos_icon(&app,row.path.data(),mode);
    if(icon&&icon!=(HICON)1) {
     row.ownedIcons=std::make_shared<BrowserDirectory::OwnedIcons>();
     row.ownedIcons->small=row.ownedIcons->large=icon;
     row.icon16=row.icon32=icon;
    }
   }
  }

  const auto display=BrowserPresentation::Utf8(row.entry.name);
  BrowserDirectory::ObserveRead(row.path.empty()?display.c_str():row.path.c_str());
  row.accessKey=VolumeCoord::ObservedPathKey(snapshot.resourceKey,leaf,row.entry.identity,row.entry.directory);
  snapshot.rows.push_back(std::move(row));
  Storage::Progress progress;
  progress.phase="Reading folder";
  progress.path=display;
  progress.utf8=true;
  job.progress(progress);
 }while(find.Next());
 if(find.Error()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed");
 StorageLocation::ValidateMedia(media);
 OwnIcons(snapshot);
 return snapshot;
}
void CHFVExplorerDoc::apply_browser_directory(const BrowserDirectory::Snapshot& snapshot,
    int type, int index, uint32_t folder, const std::string& path, bool listing){
 auto& session = GetBrowserSession();
 auto tree = session.Tree();
 HTREEITEM root = tree->GetTreeCtrl().GetSelectedItem();
 session.PresentDirectory(snapshot, type, index, folder, path, listing,
    [&](const BrowserDirectory::Row& row) {
  auto name=MacBrowser::Wide(row.entry.name);int n=WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,nullptr,0,nullptr,nullptr);std::string ansi(n,0);
  if(n)WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,&ansi[0],n,nullptr,nullptr);
  if(row.storageType==LIST_HFS)
   return tree->tree_insert(m_volumes[row.volume].m_rootitem,row.volume,const_cast<char*>(ansi.c_str()),row.id,row.parent);
  return tree->InsertWindowsFolder(root,row.volume,name,row.location,row.capabilities.create);
 });
}

void BrowserFolderReader::OwnIcons(BrowserDirectory::Snapshot& snapshot)
{
 for (auto& row : snapshot.rows) {
  if (row.ownedIcons || (!row.icon16 && !row.icon32))
   continue;
  auto icons = std::make_shared<BrowserDirectory::OwnedIcons>();
  if (row.icon16)
   icons->small = CopyIcon(row.icon16);
  if (row.icon32)
   icons->large = row.icon16 == row.icon32 ? icons->small : CopyIcon(row.icon32);
  row.icon16 = icons->small;
  row.icon32 = icons->large;
  row.ownedIcons = std::move(icons);
 }
}

void CHFVExplorerDoc::PublishWindowsFolder(int index, const std::string& path,
    const BrowserDirectory::Snapshot& snapshot,HTREEITEM item)
{
 auto& session = GetBrowserSession();
 auto parent = path;
 auto separator = path.find_last_of('\\');
 const bool root = is_fat_root_directory(const_cast<char*>(path.c_str())) != 0;
 if (!root && separator != path.npos)
  parent.resize(separator == 2 ? 3 : separator);
 else
  parent.clear();
 session.PrepareFolderPresentation(item,parent.c_str(),!root);
 apply_browser_directory(snapshot, LIST_FAT, index, 2, path, true);
 if(auto status=session.Status())status->SetPaneText(0,path.c_str());
}

bool CHFVExplorerDoc::NavigateWindowsFolder(int index, const std::string& path, bool refresh, const BrowserNavigation::Request* find)
{
 return NavigateWindowsFolder(index,VolumeCoord::Wide(path),refresh,find);
}

bool CHFVExplorerDoc::NavigateWindowsFolder(int index, const std::wstring& path, bool refresh, const BrowserNavigation::Request* find)
{
 auto& session = GetBrowserSession();
 if ((!find&&!session.UsesAsyncNavigation()) || index < 0 || index >= m_fat_count ||
     path.size() < 3 || path[1] != L':')
  return false;

 const auto finder=session.finderLayout;
 auto reader=std::make_shared<BrowserFolderReader>(FolderOptions());
 struct Listing {
  std::vector<Removable::Mounted> removable;
  std::shared_ptr<MacBrowser::Volume> cd;
  bool floppy=false;
  std::wstring path;
 };
 auto listing=std::make_shared<Listing>();
 listing->path=path;
 auto& control=session.Tree()->GetTreeCtrl();
 const auto item=control.GetSelectedItem();
 const auto root=m_fats[index].m_rootitem;
 const auto type=m_fats[index].m_type;
 const bool atRoot=path.size()==3&&path[2]==L'\\';
 const int drive=towupper(path[0])-L'A';
 const bool probeCd=!find&&atRoot&&type==DRIVE_CDROM&&!session.MacBrowser().HasCdRoot(drive);
 const bool probeRemovable=!find&&atRoot&&(type==DRIVE_REMOVABLE||type==DRIVE_FIXED);
 const auto deviceKey=(probeCd||probeRemovable)?VolumeCoord::DeviceKey(drive):std::string();
 using namespace BrowserNavigation;
 Request request=find?*find:Request{FileOperations::Location::Windows(path),
     refresh?Intent::RefreshFolder:Intent::SelectFolder,{}};
 request.folder.directory=true;
 request.mediaDrive=drive;
 request.resource=deviceKey.empty()?session.ObservedWindowsKey(path):deviceKey;
 const auto mount=session.Mounts().Lookup({BrowserMountInventory::Kind::WindowsDrive,index});
 if(!mount)return false;
 const auto retry=[this,index,path,refresh,found=find?std::optional<Request>(*find):std::nullopt]{
  NavigateWindowsFolder(index,path,refresh,found?&*found:nullptr);
 };
 session.Navigate(std::move(request),
 [reader,listing,index,drive,type,probeCd,probeRemovable,deviceKey,finder](const Request& request,Result& result,Storage::Job& job) {
  try {
   if(job.cancelled())throw Storage::Cancelled();
   if(probeCd||probeRemovable){
    if(probeRemovable){
     if(type==DRIVE_REMOVABLE&&MediaDiscovery::Floppy(drive,job)){
      VolumeCoord::Lease access({{deviceKey,VolumeCoord::Mode::Read}});
      char image[MAX_PATH];get_floppy_volume_file_name(drive,image);
      auto& volume=reader->Slots()[0];
      volume.m_file_name=image;volume.m_is_floppy=1;volume.m_free=0;
      std::shared_ptr<MacBrowser::Volume> backend;
      try{backend=MacBrowser::OpenClassic(image,":",false);}catch(const HfsPlus::Error&){}
      if(backend){
       volume.ReadMount(MacBrowser::ReadClassicMountInfo(*backend));
       reader->Revision(0)=VolumeCoord::Revision(deviceKey);
       result.listing=reader->read_classic_browser_directory(0,2,volume.m_volume_name+":",finder,0,&job,backend.get());
       result.folder=FileOperations::Location::Classic(image,(LPCSTR)(volume.m_volume_name+":"));
       result.folder.directory=true;
       result.folder.expectedCatalogId=2;
       result.folder.expectedResource=deviceKey;
       if(volume.m_mediaGenerationKnown)result.folder.BindMediaGeneration(volume.m_mediaGeneration);
       StorageLocation::ValidateMedia(result.folder,StorageLocation::FloppyProbe::FreshHandle);
       listing->floppy=true;
       return Storage::Result{};
      }
     }
     VolumeCoord::Lease access({{deviceKey,VolumeCoord::Mode::Read}});
     listing->removable=Removable::FromDriveRoot(listing->path,&job);
     if(!listing->removable.empty()){
      for(auto& mount:listing->removable)StorageLocation::ValidateMedia(FileOperations::Location::Mounted(mount.volume));
      result.folder=FileOperations::Location::Mounted(listing->removable.front().volume);
      result.folder.directory=true;
      return Storage::Result{};
     }
    }
    if(probeCd){
     listing->cd=MediaDiscovery::Optical(drive,job);
     if(listing->cd){
      result.folder=FileOperations::Location::Mounted(listing->cd);
      StorageLocation::ValidateMedia(result.folder);
      result.folder.directory=true;
      return Storage::Result{};
     }
    }
   }
   if(job.cancelled())throw Storage::Cancelled();
   if(request.intent==Intent::RefreshFolder){
    auto& path=listing->path;
    while(path.size()>3){
     auto attrs=GetFileAttributesW(BrowserPresentation::WindowsPath(path).c_str());
     if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY))break;
     const auto error=GetLastError();
     if(attrs==INVALID_FILE_ATTRIBUTES&&error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_FILE_NOT_FOUND)
      throw std::runtime_error("The Windows folder is unavailable");
     auto separator=path.find_last_of(L'\\');
     path.resize(separator<=2?3:separator);
    }
   }
   result.listing=reader->read_windows_browser_directory(index,listing->path,true,job,
       request.folder.WindowsMediaSerial());
   result.folder.SetWindows(result.listing.folderPath);
   result.folder.BindWindowsMediaSerial(result.listing.windowsMediaSerial);
   result.folder.expectedIdentity=result.listing.folderIdentity;
  }catch(const VolumeCoord::Busy&){
   if(deviceKey.empty())result.listing.resourceKey=VolumeCoord::PathKey(listing->path);
   return Storage::Result{1, ERROR_BUSY, "Volume busy"};
  }
  return Storage::Result{};
 }, [this,reader,listing,index,drive,path,item,root,deviceKey,refresh,findResult=bool(find)](Result& result) {
  auto& session=GetBrowserSession();
  auto tree=session.Tree();
  auto& control=tree->GetTreeCtrl();
  if(listing->cd){session.MacBrowser().AdoptCd(drive,CStringA(path.c_str()).GetString(),std::move(listing->cd));return;}
  if(!listing->removable.empty()){session.MacBrowser().AdoptRemovable(listing->removable,drive);return;}
  auto& snapshot=result.listing;
  if(listing->floppy){
   auto& prepared=reader->Slots()[0];
   int mounted=get_volume_index_by_filename(prepared.m_file_name);
   if(mounted<0)mounted=insert_volume(prepared.m_file_name,1,0,0,0);
   if(mounted<0){session.RejectNavigation(deviceKey,"Cannot attach the selected floppy");return;}
   auto& old=m_volumes[mounted];
   auto enabled=tree->SelectionNotificationsEnabled();
   tree->enable_selchange(0);
   prepared.m_rootitem=old.m_rootitem?old.m_rootitem:
      tree->initialize_volume(mounted,prepared.m_volume_name,2,1,0,0,0);
   tree->delete_subdir_contents(prepared.m_rootitem);
   control.SetItemText(prepared.m_rootitem,prepared.m_volume_name);
   tree->enable_selchange(enabled);
   m_volumes.ReplaceFrom(mounted,reader->Slots(),0);
   m_classicRevision[mounted]=snapshot.revision;
   for(auto& row:snapshot.rows)row.volume=mounted;
   session.PrepareFolderPresentation(m_volumes[mounted].m_rootitem,"",false);
   PublishClassicFolder(mounted,2,std::string((LPCSTR)m_volumes[mounted].m_volume_name)+":",snapshot);
   return;
  }
  const auto& shown=result.folder.WindowsPath();
  auto destination=item;
  if(findResult||_wcsicmp(shown.c_str(),path.c_str())){
   destination=tree->FindWindowsFolder(root,shown);
   if(!destination&&findResult){
    const auto* folder=tree->BrowserFolderAt(root);
    auto prefix=folder&&folder->location.IsWindows()?folder->location.WindowsPath():std::wstring();
    while(prefix.size()>3&&prefix.back()=='\\')prefix.pop_back();
    if(prefix.empty()||_wcsnicmp(shown.c_str(),prefix.c_str(),prefix.size())||
       (shown.size()>prefix.size()&&prefix.back()!='\\'&&shown[prefix.size()]!='\\'))
     throw std::runtime_error("The search result is outside the selected volume.");
    destination=root;
    auto first=prefix.size();
    if(prefix.back()=='\\')prefix.pop_back();
    while(first<shown.size()){
     if(shown[first]=='\\')++first;
     const auto end=shown.find('\\',first);
     auto leaf=shown.substr(first,end==shown.npos?end:end-first);
     if(leaf.empty())break;
     prefix+=L"\\"+leaf;
     destination=tree->InsertWindowsFolder(destination,index,leaf,FileOperations::Location::Windows(prefix),snapshot.capabilities.create);
     if(!destination)throw std::runtime_error("Cannot display the search result folder");
     first=end==shown.npos?shown.size():end;
    }
   }
   if(!destination)destination=root;
  }
  if(refresh)tree->delete_subdir_contents(destination);
  PublishWindowsFolder(index,CStringA(shown.c_str()).GetString(),snapshot,destination);
 },mount,retry);
 return true;
}
