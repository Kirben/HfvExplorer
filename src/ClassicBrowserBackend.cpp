// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LegacyEngine.h"
#include "ClassicCatalog.h"
#include "HfsPlusDisk.h"
#include "FileOperations.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "BrowserSession.h"
#include "StorageLocation.h"
#include "ReadOnlyCdBackend.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "VolumeCoordinator.h"
#include "BrowserPresentation.h"
#include "HostFileSizes.h"
#include "special.h"
#include "utils.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
}
#include "hfs/interface.h"
#include "charset.h"
#include "aspecial.h"
#include <set>
#include <fcntl.h>
#include <algorithm>
#include <stdexcept>
extern HICON get_dos_icon(CHFVExplorerApp*,char*,int);
extern int is_afp_directory(char*);
bool BrowserFolderReader::prepare_classic_browser_row( hfs_item_type *pitem, CatDataRec* folderIcon, int finder_view_mode, BrowserDirectory::Row& row )
{
	int isdir;
	hfv_size_t size_in_bytes;
	unsigned long creation_date;
	unsigned long modification_date;
	char stype[100];
	char screator[100];
	unsigned long id;
	HICON hicon16, hicon32;
	// hfs_point point;
	POINT p;
	int is_inited = 0;
	int finder_color = FINDER_COLOR_NONE;

	// Special handling for folder icon. Some folks don't
	// put the invisible flag properly on.
	if(!options_.showMacHidden) {
		if(strcmp((char *)pitem->catkeyrec.ckrCName,"Icon\x00D") == 0) {
		 	return false;
		}		
	}

	p.x = -100;
	p.y = -100;

	if(*pitem->catkeyrec.ckrCName) {
		id = 0;
		isdir = 0;
		size_in_bytes = 0;
		creation_date = 0;
		modification_date = 0;
		*stype = 0;
		*screator = 0;
		hicon16 = hicon32 = 0;

		if(pitem->catdatarec.cdrType == cdrDirRec) isdir = 1;
		if(pitem->catdatarec.cdrType == cdrThdRec) isdir = 1;
		if(isdir) {
			strcpy( stype, "Folder" );
			if(pitem->catdatarec.cdrType == cdrDirRec) {
				if((pitem->catdatarec.u.d.dirUsrInfo.frFlags & fInvisible) && !options_.showMacHidden) {
					return false;
				}
				finder_color = (pitem->catdatarec.u.d.dirUsrInfo.frFlags & kColorMask) >> 1;
				is_inited = (pitem->catdatarec.u.d.dirUsrInfo.frFlags & kHasBeenInited) != 0;
				size_in_bytes = pitem->catdatarec.u.d.dirVal;
				creation_date = pitem->catdatarec.u.d.dirCrDat;
				modification_date = pitem->catdatarec.u.d.dirMdDat;
				id = pitem->catdatarec.u.d.dirDirID;

				// Try to load folder custom icon
				if(folderIcon) {
#ifndef ZAPPER
					load_custom_icons( 
							pitem->volinx,
							folderIcon,
							&hicon16, 
							&hicon32 );
#endif
				}
				p.x = (int)(signed short)pitem->catdatarec.u.d.dirUsrInfo.frLocation.h;
				p.y = (int)(signed short)pitem->catdatarec.u.d.dirUsrInfo.frLocation.v;
			}

		} else {
			if((pitem->catdatarec.u.f.filUsrWds.fdFlags & fInvisible) && !options_.showMacHidden) {
				return false;
			}

			finder_color = (pitem->catdatarec.u.f.filUsrWds.fdFlags & kColorMask) >> 1;
			
			is_inited = (pitem->catdatarec.u.f.filUsrWds.fdFlags & kHasBeenInited) != 0;
			OSType fdType = pitem->catdatarec.u.f.filUsrWds.fdType;
			OSType fdCreator = pitem->catdatarec.u.f.filUsrWds.fdCreator;
			if(pitem->catdatarec.cdrType == cdrFilRec) {
				id = pitem->catdatarec.u.f.filFlNum;
				size_in_bytes = (hfv_size_t)pitem->catdatarec.u.f.filLgLen +
												pitem->catdatarec.u.f.filRLgLen;
				creation_date = pitem->catdatarec.u.f.filCrDat;
				modification_date = pitem->catdatarec.u.f.filMdDat;
				long_to_string( fdType,(unsigned char *)stype );
				long_to_string( fdCreator,(unsigned char *)screator );

				// First try to find custom icons
#ifndef ZAPPER
				if(pitem->catdatarec.u.f.filUsrWds.fdFlags & kHasCustomIcon) {
					load_custom_icons( 
							pitem->volinx,
							&pitem->catdatarec,
							&hicon16, 
							&hicon32 );
				}
#endif

				// Try to load from cache
				if(!hicon32) hicon32 = map_type_and_creator ( fdType, fdCreator, 0 );
				if(!hicon16) hicon16 = map_type_and_creator ( fdType, fdCreator, 1 );

				// Check if it has a bundle...
				if(!hicon16 || !hicon32) {
					process_bundle(
						pitem->volinx,
						&pitem->catdatarec,
						fdCreator
					);
					// ... and perhaps the icon is in cache now?
					if(!hicon32) hicon32 = map_type_and_creator ( fdType, fdCreator, 0 );
					if(!hicon16) hicon16 = map_type_and_creator ( fdType, fdCreator, 1 );
				}

				// File aliases
				if(!hicon32 && fdType == 'adrp')
					hicon32 = map_type_and_creator ( 'APPL', fdCreator, 0 );
				if(!hicon16 && fdType == 'adrp')
					hicon16 = map_type_and_creator ( 'APPL', fdCreator, 1 );

				// Folder aliases .. this is not correct
				if(!hicon32 && fdType == 'fdrp')
					hicon32 = map_type_and_creator ( 'APPL', fdCreator, 0 );
				if(!hicon16 && fdType == 'fdrp')
					hicon16 = map_type_and_creator ( 'APPL', fdCreator, 1 );

				p.x = (int)(signed short)pitem->catdatarec.u.f.filUsrWds.fdLocation.h;
				p.y = (int)(signed short)pitem->catdatarec.u.f.filUsrWds.fdLocation.v;
			}
		}


        row.storageType=LIST_HFS;row.volume=pitem->volinx;row.id=uint32_t(id);row.parent=uint32_t(pitem->id);
        row.entry.id=row.id;row.entry.parent=row.parent;row.entry.resolvedId=row.id;
        row.entry.name=BrowserDirectory::FromAnsi((char*)pitem->catkeyrec.ckrCName);row.entry.directory=isdir!=0;
        row.entry.created=creation_date;row.entry.modified=modification_date;row.entry.datesValid=true;
        row.created=BrowserDirectory::MacTime(creation_date);row.modified=BrowserDirectory::MacTime(modification_date);
        if(isdir){row.directoryItems=size_in_bytes;row.countKnown=true;}else{
            row.entry.forks[0].size=pitem->catdatarec.u.f.filLgLen;row.entry.forks[1].size=pitem->catdatarec.u.f.filRLgLen;
        }
        memcpy(row.entry.finder.data(),stype,(std::min)(strlen(stype),size_t(4)));
        memcpy(row.entry.finder.data()+4,screator,(std::min)(strlen(screator),size_t(4)));
        unsigned flags=isdir?pitem->catdatarec.u.d.dirUsrInfo.frFlags:pitem->catdatarec.u.f.filUsrWds.fdFlags;
        row.entry.finder[8]=BYTE(flags>>8);row.entry.finder[9]=BYTE(flags);
        row.icon16=hicon16;row.icon32=hicon32;row.point=p;row.initialized=is_inited!=0;row.finderMode=finder_view_mode;
        auto& volume=slots_[pitem->volinx];DWORD attributes=(volume.m_is_cd||volume.m_is_floppy)?INVALID_FILE_ATTRIBUTES:GetFileAttributesA(volume.m_file_name);
        bool writable=!volume.m_is_cd&&(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_READONLY));
        row.capabilities.create=row.capabilities.rename=row.capabilities.remove=writable;
        return true;
    }
    return false;
}

BrowserDirectory::Snapshot BrowserFolderReader::read_classic_browser_directory(int volinx,long id,CString doc_name,bool finder,long parentId,Storage::Job* job,MacBrowser::Volume* mounted)
{
 BrowserDirectory::ObserveRead("classic-directory");
 BrowserDirectory::Snapshot snapshot;
 snapshot.capabilitiesKnown=true;
 auto& selected=slots_[volinx];
 const std::string image=(LPCSTR)selected.m_file_name;
 std::string directory=(LPCSTR)doc_name;
 snapshot.resourceKey=VolumeCoord::ImageKey(image);
 VolumeCoord::Lease read({{snapshot.resourceKey,VolumeCoord::Mode::Read}});
 snapshot.revision=VolumeCoord::Revision(snapshot.resourceKey);

 const DWORD attributes=(selected.m_is_cd||selected.m_is_floppy)?INVALID_FILE_ATTRIBUTES:GetFileAttributesA(image.c_str());
 const bool writable=!selected.m_is_cd&&(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_READONLY));
 snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=writable;

 auto opened=mounted?nullptr:MacBrowser::OpenClassic(image,id==2?":":directory,false);
 auto* volume=mounted?mounted:opened.get();
 if(opened){
  auto location=FileOperations::Location::Classic(image,directory);
  if(selected.m_mediaGenerationKnown)location.BindMediaGeneration(selected.m_mediaGeneration);
  const auto info=MacBrowser::ReadClassicMountInfo(*volume);
  StorageLocation::ValidateClassicGeneration(location,info.generation);
  selected.ReadMount(info);
  if(id==2)directory=std::string(selected.m_volume_name.GetString())+":";
 }
 auto records=MacBrowser::ReadClassicCatalog(*volume,job);
 snapshot.finderMode=FINDER_VIEW_MODE_ICON;
 if(finder&&parentId){
  const auto folder=volume->Stat(2);
  snapshot.finderMode=get_finder_view_mode(HfsPlus::Disk::U16(folder.finder.data()+14));
 }
 struct ResetArtwork {
  MacBrowser::Volume*& ptr;
  ~ResetArtwork(){ptr=nullptr;}
 } resetArtwork{artworkVolume_};
 artworkVolume_=volume;
 LegacyEngine::Scope finderResources;
 int x=INT_MAX,y=INT_MAX;
 for(auto& record:records){
  if(job&&job->cancelled())throw Storage::Cancelled();
  hfs_item_type item{};
  item.volinx=volinx;
  item.id=id;
  auto name=reinterpret_cast<char*>(item.catkeyrec.ckrCName);
  strcpy_s(name,sizeof(item.catkeyrec.ckrCName),record.name.c_str());
  static_assert(sizeof(CatDataRec)==sizeof(MacBrowser::ClassicCatalogEntry::PackedRecord));
  memcpy(&item.catdatarec,record.data.data(),sizeof(item.catdatarec));
  CatDataRec folderIcon{};
  if(record.folderIcon)memcpy(&folderIcon,record.folderIcon->data(),sizeof(folderIcon));
  mac_to_pc_charset(item.catkeyrec.ckrCName);
  BrowserDirectory::ObserveRead(name);
  BrowserDirectory::Row row;
  if(!prepare_classic_browser_row(&item,record.folderIcon?&folderIcon:nullptr,snapshot.finderMode,row))
   continue;
  row.path=directory+(directory.empty()||directory.back()!=':'?":":"")+name;
  if(finder&&(row.point.x!=-1||row.point.y!=-1)&&
     (row.initialized||is_inside_magic(row.point.x,row.point.y))){
   x=(std::min)(x,unmagic(row.point.x));
   y=(std::min)(y,unmagic(row.point.y));
  }
  snapshot.rows.push_back(std::move(row));
 }
 if(x!=INT_MAX)snapshot.originX=x;
 if(y!=INT_MAX)snapshot.originY=y;
 OwnIcons(snapshot);
 if(opened)volume->Close();
 revisions_[volinx]=snapshot.revision;
 return snapshot;
}

namespace {
using MacBrowser::ClassicAncestor;
}

bool CHFVExplorerDoc::NavigateClassicFolder(int index,uint32_t id,bool refresh,const BrowserNavigation::Request* find)
{
 auto& session=GetBrowserSession();
 if((!find&&!session.UsesAsyncNavigation())||index<0||index>=m_hfs_count)return false;
 auto tree=session.Tree();
 const auto item=tree->GetTreeCtrl().GetSelectedItem();
 auto& selected=m_volumes[index];
 const auto root=selected.m_rootitem;
 if((!item&&!find)||!root||selected.m_free)return false;
 using namespace BrowserNavigation;
 CString directory=tree->get_directory_name(root,index,id);
 if(directory.IsEmpty())directory=selected.m_volume_name;
 if(directory.IsEmpty()&&!find)return false;
 std::string path=(LPCSTR)directory;
 path+=':';
 const auto parent=tree->get_parent_id(root,index,id);
 const auto image=selected.m_file_name;
 const auto key=VolumeCoord::ImageKey(std::string((LPCSTR)image));
 Request request=find?*find:Request{FileOperations::Location::Classic((LPCSTR)image,path),
     refresh?Intent::RefreshFolder:Intent::SelectFolder,key};
 if(find&&request.folder.expectedResource!=key)return false;
 request.folder.directory=true;
 request.folder.expectedCatalogId=id;
 request.folder.expectedResource=key;
 if(request.folder.IsPhysical())request.mediaDrive=request.folder.Drive();
 if(!find&&selected.m_mediaGenerationKnown)
  request.folder.BindMediaGeneration(selected.m_mediaGeneration);
 auto reader=std::make_shared<BrowserFolderReader>(FolderOptions());
 auto& volume=reader->Slots()[index];
 volume.m_file_name=image;
 volume.m_is_floppy=selected.m_is_floppy;
 volume.m_is_cd=selected.m_is_cd;
 volume.m_is_hd=selected.m_is_hd;
 volume.m_is_removable=selected.m_is_removable;
 volume.m_free=0;
 const auto finder=session.finderLayout;
 auto route=std::make_shared<std::vector<ClassicAncestor>>();
 const auto mount=session.Mounts().Lookup({BrowserMountInventory::Kind::ClassicVolume,index});
 if(!mount)return false;
 const auto retry=[this,index,id,refresh,found=find?std::optional<Request>(*find):std::nullopt]{
  if(!NavigateClassicFolder(index,id,refresh,found?&*found:nullptr))
   GetBrowserSession().RejectNavigation(found?found->resource:std::string(),"The classic HFS volume changed.");
 };
 session.Navigate(std::move(request),
 [reader,route,index,finder,parent](const Request& request,Result& result,Storage::Job& job){
  try{
   VolumeCoord::Lease lease({{request.resource,VolumeCoord::Mode::Read}});
   if(job.cancelled())throw Storage::Cancelled();
   auto& volume=reader->Slots()[index];
   StorageLocation::ValidateImage(VolumeCoord::Wide(request.folder.ClassicImage()),request.resource);
   auto backend=MacBrowser::OpenClassic(request.folder.ClassicImage(),":",false);
   volume.ReadMount(MacBrowser::ReadClassicMountInfo(*backend));
   StorageLocation::ValidateClassicGeneration(request.folder,volume.m_mediaGenerationKnown?
       std::optional<DWORD>(volume.m_mediaGeneration):std::nullopt);
   reader->Revision(index)=VolumeCoord::Revision(request.resource);
   auto id=request.folder.expectedCatalogId;
   const bool readRoute=request.intent!=Intent::SelectFolder;
   std::string directory=id==2?std::string((LPCSTR)volume.m_volume_name)+":":request.folder.ClassicPath();
   if(readRoute&&id!=2){
    if(!id){
     MacBrowser::SelectClassicCatalogFolder(*backend,directory);
     id=backend->Stat(2).resolvedId;
    }
    *route=MacBrowser::ReadClassicAncestors(*backend,id);
    directory=std::string((LPCSTR)volume.m_volume_name)+":";
    for(const auto& step:*route)directory+=step.name+":";
   }
   if(directory.size()>=MAX_PATH)throw std::runtime_error("The classic HFS path exceeds the supported length");
   const auto parentId=route->empty()?parent:route->back().parent;
   if(id!=2)MacBrowser::SelectClassicCatalogFolder(*backend,directory);
   result.listing=reader->read_classic_browser_directory(index,id,directory.c_str(),finder,parentId,&job,backend.get());
   if(result.listing.resourceKey!=request.resource)
    return Storage::Result{1,ERROR_MEDIA_CHANGED,"The classic HFS image was replaced."};
   result.folder.SetClassic(request.folder.ClassicImage(),directory);
   result.folder.expectedCatalogId=id;
   if(volume.m_mediaGenerationKnown)result.folder.BindMediaGeneration(volume.m_mediaGeneration);
   StorageLocation::ValidateMedia(result.folder,StorageLocation::FloppyProbe::FreshHandle);
   StorageLocation::ValidateImage(VolumeCoord::Wide(result.folder.ClassicImage()),request.resource);
   return Storage::Result{};
  }catch(const VolumeCoord::Busy&){return Storage::Result{1,ERROR_BUSY,"Classic HFS volume busy"};}
 },[this,reader,route,index,item,root,refresh,path,findResult=bool(find),parent](Result& result){
  auto& session=GetBrowserSession();
  auto tree=session.Tree();
  auto& control=tree->GetTreeCtrl();
  auto& volume=reader->Slots()[index];
  volume.m_rootitem=root;
  m_volumes.ReplaceFrom(index,reader->Slots(),index);
  m_classicRevision[index]=result.listing.revision;
  control.SetItemText(root,m_volumes[index].m_volume_name);
  const auto& shown=result.folder.ClassicPath();
  auto target=item;
  const auto enabled=tree->SelectionNotificationsEnabled();
  tree->enable_selchange(0);
  if(findResult||(refresh&&_stricmp(path.c_str(),shown.c_str()))){
   // Build only the confirmed ancestry for the destination.
   if(refresh)tree->delete_subdir_contents(root);
   target=root;
   for(auto& step:*route)target=tree->tree_insert(root,index,step.name.data(),step.id,step.parent);
  }
  if(refresh)tree->delete_subdir_contents(target);
  tree->enable_selchange(enabled);
  const auto parentId=route->empty()?parent:route->back().parent;
  const auto parentPath=parentId?tree->get_directory_name(root,index,parentId):CString();
  session.PrepareFolderPresentation(target,parentPath,parentId!=0);
  PublishClassicFolder(index,result.folder.expectedCatalogId,shown,result.listing);
 },mount,retry,[this,index,root,initial=find&&find->tryNextStartupRoot]{
  if(!initial)return false;
  auto& volume=m_volumes[index];
  volume.m_volume_name.Empty();
  volume.m_file_name.Empty();
  volume.m_free=1;
  volume.m_rootitem=nullptr;
  GetBrowserSession().Tree()->GetTreeCtrl().DeleteItem(root);
  return NavigateInitialFolder(index+1);
 });
 return true;
}
