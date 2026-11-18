// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LegacyEngine.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "PhysicalRemovable.h"
#include "Floppy.h"
#include "BackgroundCopies.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "BrowserPresentation.h"
#include "HostFileSizes.h"
#include "charset.h"
#include "aspecial.h"
#include "utils.h"
#include <set>
#include <stdexcept>
extern HICON get_dos_icon(CHFVExplorerApp*,char*,int);
extern int is_afp_directory(char*);
namespace {
struct Find {
 HANDLE handle=INVALID_HANDLE_VALUE;
 ~Find(){if(handle!=INVALID_HANDLE_VALUE)FindClose(handle);}
};
std::string key(std::string name){if(!name.empty())CharUpperBuffA(&name[0],DWORD(name.size()));return name;}
}
BrowserDirectory::Snapshot CHFVExplorerDoc::read_windows_browser_directory(int volume,const char* directory,bool listing,Storage::Job& job){
 BrowserDirectory::ObserveRead(directory);
 if(BackgroundCopies::Active()){
  BrowserDirectory::Snapshot out;out.resourceKey=VolumeCoord::PathKey(VolumeCoord::Wide(directory));out.revision=VolumeCoord::Revision(out.resourceKey);out.capabilitiesKnown=true;
  out.folderPath=VolumeCoord::Wide(directory);try{out.folderIdentity=MacBrowser::DescribeWindowsFile(out.folderPath).identity;}catch(const std::exception&){}
  char root[]={directory[0],':','\\',0};out.capabilities.create=out.capabilities.rename=out.capabilities.remove=GetDriveTypeA(root)!=DRIVE_CDROM;
  std::string prefix=directory;if(!prefix.empty()&&prefix.back()!='\\')prefix+='\\';
  Find find;WIN32_FIND_DATAA info{};find.handle=FindFirstFileA((prefix+"*.*").c_str(),&info);
  if(find.handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return out;throw std::runtime_error("Cannot enumerate Windows folder");}
  do{
   if(job.cancelled())throw Storage::Cancelled();if(!strcmp(info.cFileName,".")||!strcmp(info.cFileName,"..")||(!m_pc_show_invisibles&&(info.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN)))continue;
   if(info.cFileName[0]=='%'&&GetFileAttributesA((prefix+(info.cFileName+1)).c_str())!=INVALID_FILE_ATTRIBUTES){try{if(MacBrowser::WindowsResourceCompanion(VolumeCoord::Wide(prefix+info.cFileName)))continue;}catch(const std::exception&){}}
   BrowserDirectory::Row row;row.storageType=LIST_FAT;row.volume=volume;row.capabilities=out.capabilities;row.path=prefix+info.cFileName;
   row.entry.name=BrowserDirectory::FromAnsi(info.cFileName);row.entry.directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;row.entry.forks[0].size=row.entry.directory?0:HostDirectoryEntrySize(info);
   row.created=BrowserDirectory::Ticks(info.ftCreationTime);row.modified=BrowserDirectory::Ticks(info.ftLastWriteTime);
   try{row.entry=MacBrowser::DescribeWindowsFile(VolumeCoord::Wide(row.path));}catch(const std::exception&){/* Staged replacements may be briefly locked; keep OS metadata. */}
   row.accessKey=VolumeCoord::ObservedPathKey(out.resourceKey,VolumeCoord::Wide(info.cFileName),row.entry.identity,row.entry.directory);
   out.rows.push_back(std::move(row));
  }while(FindNextFileA(find.handle,&info));
  if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed");
return out;
 }
 LegacyEngine::Scope engine;
 BrowserDirectory::Snapshot snapshot;snapshot.resourceKey=VolumeCoord::PathKey(VolumeCoord::Wide(directory));VolumeCoord::Lease read({{snapshot.resourceKey,VolumeCoord::Mode::Browse}});snapshot.revision=VolumeCoord::Revision(snapshot.resourceKey);auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 snapshot.folderPath=VolumeCoord::Wide(directory);try{snapshot.folderIdentity=MacBrowser::DescribeWindowsFile(snapshot.folderPath).identity;}catch(const std::exception&){}
 std::string prefix=directory;if(prefix.empty())throw std::runtime_error("Select a Windows folder");if(prefix.back()!='\\')prefix+='\\';
 bool afp=is_afp_directory(const_cast<char*>(directory))!=0;
 char root[]={directory[0],':',char(92),0};bool writable=GetDriveTypeA(root)!=DRIVE_CDROM;
 std::set<std::string> ignored;
 snapshot.capabilitiesKnown=true;snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=writable;
 auto append=[&](const char* display,const std::string& path,bool folder,const WIN32_FIND_DATAA& info,HICON small,HICON large,const char* type,const char* creator,bool companions,bool own=false){
  BrowserDirectory::ObserveRead(path.c_str());
  BrowserDirectory::Row row;row.storageType=LIST_FAT;row.volume=volume;row.path=path;
  row.entry.name=BrowserDirectory::FromAnsi(display);row.entry.directory=folder;
  row.created=BrowserDirectory::Ticks(info.ftCreationTime);row.modified=BrowserDirectory::Ticks(info.ftLastWriteTime);
  if(folder){row.countKnown=false;}
  else row.entry.forks[0].size=HostDirectoryEntrySize(info);
  memcpy(row.entry.finder.data(),type,(std::min)(strlen(type),size_t(4)));
  memcpy(row.entry.finder.data()+4,creator,(std::min)(strlen(creator),size_t(4)));
  if(companions&&!folder){auto normalized=MacBrowser::DescribeWindowsFile(MacBrowser::Wide(BrowserDirectory::FromAnsi(path.c_str())));
   normalized.name=row.entry.name;row.entry=std::move(normalized);
  }
  if(row.entry.identity.empty()){try{row.entry.identity=MacBrowser::DescribeWindowsFile(VolumeCoord::Wide(path)).identity;}catch(const std::exception&){/* Unidentifiable entries are revalidated before a mutation is queued. */}}
  row.icon16=small;row.icon32=large;
  if(own&&(small||large)){row.ownedIcons=std::make_shared<BrowserDirectory::OwnedIcons>();row.ownedIcons->small=small;row.ownedIcons->large=large;}row.capabilities.create=row.capabilities.rename=row.capabilities.remove=writable;
  auto leaf=path.substr(path.find_last_of("\\/")+1);row.accessKey=VolumeCoord::ObservedPathKey(snapshot.resourceKey,VolumeCoord::Wide(leaf),row.entry.identity,row.entry.directory);
  snapshot.rows.push_back(std::move(row));
 };
 // Keep the classic AppleDouble/AFP icon and filename conventions in this backend.
 // The view receives only a normalized row and its operation binding.
 {
  Find find;WIN32_FIND_DATAA info{};find.handle=FindFirstFileA((prefix+"%*.*").c_str(),&info);
  if(find.handle==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot read Windows folder (Windows error "+std::to_string(GetLastError())+").");
  if(find.handle!=INVALID_HANDLE_VALUE)do{
   if(job.cancelled())throw Storage::Cancelled();HICON small=nullptr,large=nullptr;char name[MAX_PATH],type[20]{},creator[20]{};int show=0;
   if(get_apple_double(prefix.c_str(),info.cFileName,&small,&large,name,type,creator,&show)){
    ignored.insert(key(std::string("%")+name));ignored.insert(key(name));
    if(show){std::string path=prefix+name;bool folder=isdirectory(path.c_str())!=0;
     remove_ardi_special_chars((unsigned char*)name);mac_to_pc_charset((unsigned char*)name);
     WIN32_FILE_ATTRIBUTE_DATA data{};WIN32_FIND_DATAA actual=info;
     if(GetFileAttributesExA(path.c_str(),GetFileExInfoStandard,&data)){actual.ftCreationTime=data.ftCreationTime;actual.ftLastWriteTime=data.ftLastWriteTime;actual.nFileSizeHigh=data.nFileSizeHigh;actual.nFileSizeLow=data.nFileSizeLow;}
     append(name,path,folder,actual,small,large,type,creator,true);
    }
   }
  }while(FindNextFileA(find.handle,&info));
  if(find.handle!=INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows companion enumeration failed");
 }
 DWORD deadline=app.m_dosicons!=DI_NO_ICONS&&app.m_fat_icon_time_limit>0?GetTickCount()+DWORD(app.m_fat_icon_time_limit)*1000:0;
 Find find;WIN32_FIND_DATAA info{};find.handle=FindFirstFileA((prefix+"*.*").c_str(),&info);
 if(find.handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return snapshot;throw std::runtime_error("Cannot enumerate Windows folder");}
 do{
  if(job.cancelled())throw Storage::Cancelled();bool folder=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;bool owned=false;
  if((folder&&(!strcmp(info.cFileName,".")||!strcmp(info.cFileName,"..")))||ignored.count(key(info.cFileName))||(!m_pc_show_invisibles&&(info.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN)))continue;
  dos_lfn_fdata(&info);std::string path=prefix+info.cFileName;HICON small=nullptr,large=nullptr;char type[20]{},creator[20]{},name[MAX_PATH];int show=1;
  if(!folder&&listing){
   if(afp)get_afp(prefix.c_str(),info.cFileName,&small,&large,name,type,creator,&show);
   if(!large){int mode=deadline&&LONG(GetTickCount()-deadline)>=0?DI_NO_ICONS:app.m_dosicons;small=get_dos_icon(&app,const_cast<char*>(path.c_str()),mode);if(small==(HICON)1)small=nullptr;large=small;owned=true;}
  }
  append(info.cFileName,path,folder,info,small,large,type,creator,false,owned);
  Storage::Progress progress;progress.phase="Reading folder";progress.path=path;job.progress(progress);
 }while(FindNextFileA(find.handle,&info));
 if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Windows folder enumeration failed");
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
  return tree->tree_fat_insert(root,row.volume,const_cast<char*>(ansi.c_str()),const_cast<char*>(row.path.c_str()));
 });
}

void CHFVExplorerDoc::CopyNavigationIcons(BrowserDirectory::Snapshot& snapshot)
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
    const BrowserDirectory::Snapshot& snapshot)
{
 auto& session = GetBrowserSession();
 auto parent = path;
 auto separator = path.find_last_of('\\');
 const bool root = is_fat_root_directory(const_cast<char*>(path.c_str())) != 0;
 if (!root && separator != path.npos)
  parent.resize(separator == 2 ? 3 : separator);
 else
  parent.clear();
 session.List()->initialize_volume(parent.c_str(), root ? 0 : ID_GOTO_PARENT);
 apply_browser_directory(snapshot, LIST_FAT, index, 2, path, true);
 if(auto status=session.Status())status->SetPaneText(0,path.c_str());
}

bool CHFVExplorerDoc::NavigateWindowsFolder(int index, const std::string& path, bool refresh, const BrowserNavigation::Request* find)
{
 auto& session = GetBrowserSession();
 if ((!find&&!session.UsesAsyncNavigation()) || index < 0 || index >= m_fat_count ||
     path.size() < 3 || path[1] != ':')
  return false;

 // The icon and AppleDouble readers use document-owned scratch state. Give
 // the job an unattached reader so closing the browser never invalidates it.
 auto reader = std::shared_ptr<CHFVExplorerDoc>(new CHFVExplorerDoc(false));
 reader->m_pc_show_invisibles = m_pc_show_invisibles;
 reader->m_mac_show_invisibles = m_mac_show_invisibles;
 reader->GetBrowserSession().finderLayout=session.finderLayout;
 struct Listing {
  std::vector<Removable::Mounted> removable;
  std::shared_ptr<MacBrowser::Volume> cd;
  bool floppy=false;
  std::string path;
 };
 auto listing=std::make_shared<Listing>();
 listing->path=path;
 auto& control=session.Tree()->GetTreeCtrl();
 const auto item=control.GetSelectedItem();
 const auto root=m_fats[index].m_rootitem;
 const auto type=m_fats[index].m_type;
 const bool atRoot=is_fat_root_directory(const_cast<char*>(path.c_str()))!=0;
 const int drive=toupper(static_cast<unsigned char>(path[0]))-'A';
 const bool probeCd=!find&&atRoot&&type==DRIVE_CDROM&&!session.MacBrowser().HasCdRoot(drive);
 const bool probeRemovable=!find&&atRoot&&(type==DRIVE_REMOVABLE||type==DRIVE_FIXED);
 const auto deviceKey=(probeCd||probeRemovable)?VolumeCoord::DeviceKey(drive):std::string();
 using namespace BrowserNavigation;
 Request request=find?*find:Request{FileOperations::Location::Windows(VolumeCoord::Wide(path)),
     refresh?Intent::RefreshFolder:Intent::SelectFolder,{}};
 request.folder.directory=true;
 request.mediaDrive=drive;
 request.resource=deviceKey.empty()?session.ObservedWindowsKey(path):deviceKey;
 const auto mountCurrent=[this,index,root,drive]{
  return index<m_fat_count&&m_fats[index].m_rootitem==root&&toupper((unsigned char)m_fats[index].m_letter)-'A'==drive;
 };
 const auto retry=[this,index,path,refresh,found=find?std::optional<Request>(*find):std::nullopt]{
  NavigateWindowsFolder(index,path,refresh,found?&*found:nullptr);
 };
 session.Navigate(std::move(request),
 [reader,listing,index,drive,type,probeCd,probeRemovable,deviceKey](const Request& request,Result& result,Storage::Job& job) {
  try {
   if(job.cancelled())throw Storage::Cancelled();
   if(probeCd||probeRemovable){
    VolumeCoord::Lease access({{deviceKey,VolumeCoord::Mode::Read}});
    if(probeRemovable){
     if(type==DRIVE_REMOVABLE&&is_hfs_floppy_present(drive)){
      char image[MAX_PATH];get_floppy_volume_file_name(drive,image);
      auto& volume=reader->m_volumes[0];
      volume.m_file_name=image;volume.m_is_floppy=1;volume.m_free=0;
      if(volume.open()){
       reader->m_classicRevision[0]=VolumeCoord::Revision(deviceKey);
       result.listing=reader->read_classic_browser_directory(0,2,volume.m_volume_name+":",reader->GetBrowserSession().finderLayout,0,&job);
       ValidateNavigationMedia(volume);
       result.folder=FileOperations::Location::Classic(image,(LPCSTR)(volume.m_volume_name+":"));
       result.folder.directory=true;
       result.folder.expectedCatalogId=2;
       result.folder.expectedResource=deviceKey;
       if(volume.m_mediaGenerationKnown)result.folder.BindMediaGeneration(volume.m_mediaGeneration);
       reader->CopyNavigationIcons(result.listing);
       listing->floppy=true;
       return Storage::Result{};
      }
     }
     listing->removable=Removable::FromDriveRoot(VolumeCoord::Wide(listing->path));
     if(!listing->removable.empty()){
      for(auto& mount:listing->removable)mount.volume->ValidateMedia();
      result.folder=FileOperations::Location::Mounted(listing->removable.front().volume);
      result.folder.directory=true;
      return Storage::Result{};
     }
    }
    if(probeCd){
     listing->cd=MacBrowser::OpenCdRoot(listing->path.c_str());
     if(listing->cd){
      listing->cd->ValidateMedia();
      result.folder=FileOperations::Location::Mounted(listing->cd);
      result.folder.directory=true;
      return Storage::Result{};
     }
    }
   }
   if(job.cancelled())throw Storage::Cancelled();
   if(request.intent==Intent::RefreshFolder){
    auto& path=listing->path;
    while(path.size()>3){
     auto attrs=GetFileAttributesA(path.c_str());
     if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY))break;
     const auto error=GetLastError();
     if(attrs==INVALID_FILE_ATTRIBUTES&&error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_FILE_NOT_FOUND)
      throw std::runtime_error("The Windows folder is unavailable");
     auto separator=path.find_last_of('\\');
     path.resize(separator<=2?3:separator);
    }
   }
   char driveRoot[]={char('A'+drive),':',char(92),0};
   DWORD serial=0;
   const bool checkSerial=(type==DRIVE_REMOVABLE||type==DRIVE_CDROM)&&
      GetVolumeInformationA(driveRoot,nullptr,0,&serial,nullptr,nullptr,nullptr,0);
   if(request.windowsMediaSerial&&(!checkSerial||serial!=*request.windowsMediaSerial))
    return Storage::Result{1,ERROR_MEDIA_CHANGED,"The Windows media changed since the search."};
   result.listing=reader->read_windows_browser_directory(index,listing->path.c_str(),true,job);
   if(checkSerial){
    DWORD after=0;
    if(!GetVolumeInformationA(driveRoot,nullptr,0,&after,nullptr,nullptr,nullptr,0)||serial!=after)
     return Storage::Result{1,ERROR_MEDIA_CHANGED,"The selected media changed during navigation"};
   }
   result.folder.SetWindows(result.listing.folderPath);
   result.folder.expectedIdentity=result.listing.folderIdentity;
   reader->CopyNavigationIcons(result.listing);
  }catch(const VolumeCoord::Busy&){
   if(deviceKey.empty())result.listing.resourceKey=VolumeCoord::PathKey(VolumeCoord::Wide(listing->path));
   return Storage::Result{1, ERROR_BUSY, "Volume busy"};
  }
  return Storage::Result{};
 }, [this,reader,listing,index,drive,path,item,root,deviceKey,refresh,findResult=bool(find)](Result& result) {
  auto& session=GetBrowserSession();
  auto tree=session.Tree();
  auto& control=tree->GetTreeCtrl();
  if(listing->cd){session.MacBrowser().AdoptCd(drive,path,std::move(listing->cd),true);return;}
  if(!listing->removable.empty()){session.MacBrowser().AdoptRemovable(listing->removable,true);return;}
  auto& snapshot=result.listing;
  if(listing->floppy){
   auto& prepared=reader->m_volumes[0];
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
   control.SelectItem(prepared.m_rootitem);
   tree->enable_selchange(enabled);
   m_volumes.ReplaceFrom(mounted,reader->m_volumes,0);
   m_classicRevision[mounted]=snapshot.revision;
   for(auto& row:snapshot.rows)row.volume=mounted;
   session.List()->initialize_volume("",0);
   PublishClassicFolder(mounted,2,std::string((LPCSTR)m_volumes[mounted].m_volume_name)+":",snapshot);
   return;
  }
  const auto shown=result.folder.LegacyPath();
  auto destination=item;
  if(findResult||_stricmp(shown.c_str(),path.c_str())){
   destination=tree->tree_find_fat(root,const_cast<char*>(shown.c_str()));
   if(!destination&&findResult){
    auto binding=reinterpret_cast<lparam_struct*>(control.GetItemData(root));
    std::string prefix=binding?binding->path:"";
    while(prefix.size()>3&&prefix.back()=='\\')prefix.pop_back();
    if(prefix.empty()||_strnicmp(shown.c_str(),prefix.c_str(),prefix.size())||
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
     prefix+="\\"+leaf;
     destination=tree->tree_fat_insert(destination,index,leaf.data(),prefix.data());
     first=end==shown.npos?shown.size():end;
    }
   }
   if(!destination)destination=root;
   auto enabled=tree->SelectionNotificationsEnabled();
   tree->enable_selchange(0);
   control.SelectItem(destination);
   control.EnsureVisible(destination);
   tree->enable_selchange(enabled);
  }
  if(refresh)tree->delete_subdir_contents(destination);
  PublishWindowsFolder(index,shown,snapshot);
 },mountCurrent,retry);
 return true;
}
