// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "LegacyEngine.h"
#include "FileOperations.h"
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
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
#include <algorithm>
#include <stdexcept>
extern HICON get_dos_icon(CHFVExplorerApp*,char*,int);
extern int is_afp_directory(char*);
bool CHFVExplorerDoc::prepare_classic_browser_row( hfs_item_type *pitem, int finder_view_mode, BrowserDirectory::Row& row )
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
	if(!m_mac_show_invisibles) {
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
				if((pitem->catdatarec.u.d.dirUsrInfo.frFlags & fInvisible) && !m_mac_show_invisibles) {
					return false;
				}
				finder_color = (pitem->catdatarec.u.d.dirUsrInfo.frFlags & kColorMask) >> 1;
				is_inited = (pitem->catdatarec.u.d.dirUsrInfo.frFlags & kHasBeenInited) != 0;
				size_in_bytes = pitem->catdatarec.u.d.dirVal;
				creation_date = pitem->catdatarec.u.d.dirCrDat;
				modification_date = pitem->catdatarec.u.d.dirMdDat;
				id = pitem->catdatarec.u.d.dirDirID;

				// Try to load folder custom icon
				CatDataRec *pIcon13catdatarec;
				pIcon13catdatarec = HFSIFACE_get_last_Icon13();
				if(pIcon13catdatarec) {
#ifndef ZAPPER
					load_custom_icons( 
							pitem->volinx,
							pIcon13catdatarec,
							&hicon16, 
							&hicon32 );
#endif
				}
				p.x = (int)(signed short)pitem->catdatarec.u.d.dirUsrInfo.frLocation.h;
				p.y = (int)(signed short)pitem->catdatarec.u.d.dirUsrInfo.frLocation.v;
			}

		} else {
			if((pitem->catdatarec.u.f.filUsrWds.fdFlags & fInvisible) && !m_mac_show_invisibles) {
				return false;
			}

			finder_color = (pitem->catdatarec.u.f.filUsrWds.fdFlags & kColorMask) >> 1;
			
			is_inited = (pitem->catdatarec.u.f.filUsrWds.fdFlags & kHasBeenInited) != 0;
			OSType fdType = pitem->catdatarec.u.f.filUsrWds.fdType;
			OSType fdCreator = pitem->catdatarec.u.f.filUsrWds.fdCreator;
			if(pitem->catdatarec.cdrType == cdrFilRec) {
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
        auto& volume=m_volumes[pitem->volinx];DWORD attributes=(volume.m_is_cd||volume.m_is_floppy)?INVALID_FILE_ATTRIBUTES:GetFileAttributesA(volume.m_file_name);
        bool writable=!volume.m_is_cd&&(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_READONLY));
        row.capabilities.create=row.capabilities.rename=row.capabilities.remove=writable;
        return true;
    }
    return false;
}

BrowserDirectory::Snapshot CHFVExplorerDoc::read_classic_browser_directory(int volinx,long id,CString doc_name,bool finder,long parentId)
{
 LegacyEngine::Scope engine;BrowserDirectory::ObserveRead("classic-directory");
 BrowserDirectory::Snapshot snapshot;snapshot.capabilitiesKnown=true;
 snapshot.resourceKey=VolumeCoord::ImageKey(std::string((LPCSTR)m_volumes[volinx].m_file_name));VolumeCoord::Lease read({{snapshot.resourceKey,VolumeCoord::Mode::Read}});snapshot.revision=VolumeCoord::Revision(snapshot.resourceKey);
 if(m_classicRevision[volinx]!=snapshot.revision){m_volumes[volinx].close();if(!m_volumes[volinx].open())throw std::runtime_error("Cannot reload changed classic HFS metadata");m_classicRevision[volinx]=snapshot.revision;}
 auto& selectedVolume=m_volumes[volinx];DWORD sourceAttributes=(selectedVolume.m_is_cd||selectedVolume.m_is_floppy)?INVALID_FILE_ATTRIBUTES:GetFileAttributesA(selectedVolume.m_file_name);
 bool writable=!selectedVolume.m_is_cd&&(sourceAttributes==INVALID_FILE_ATTRIBUTES||!(sourceAttributes&FILE_ATTRIBUTE_READONLY));
 snapshot.capabilities.create=snapshot.capabilities.rename=snapshot.capabilities.remove=writable;
 hfs_item_type item;int ret;char *volpath,*path;int finder_view_mode=FINDER_VIEW_MODE_ICON;
 auto pApp=static_cast<CHFVExplorerApp*>(AfxGetApp());
 HFSIFACE_get_dir_flush();
 struct Flush{~Flush(){HFSIFACE_get_dir_flush();}}flush;
 std::string image=(LPCSTR)m_volumes[volinx].m_file_name,directory=(LPCSTR)doc_name;
 volpath=const_cast<char*>(image.c_str());path=const_cast<char*>(directory.c_str());

	// Find out original Mac view mode
	if(finder) {
		char parent_dir[_MAX_PATH], this_name[_MAX_PATH], *ptmp;
		strcpy( parent_dir, path );
		ptmp = strrchr( parent_dir, ':' );
		if(ptmp) { // always
			*ptmp = 0;
			ptmp = strrchr( parent_dir, ':' );
			if(ptmp) {
				ptmp++;
				strcpy( this_name, ptmp );
				*ptmp = 0;
				memset( &item, 0, sizeof(item) );
				item.volinx = volinx;
				item.id = parentId;
				ret = HFSIFACE_get_dir_first_readonly( volpath, parent_dir, &item );
				if (ret == 0) {
					while (ret == 0) {
						mac_to_pc_charset( item.catkeyrec.ckrCName );
						if(strcmp((char*)item.catkeyrec.ckrCName,this_name) == 0) {
							finder_view_mode = get_finder_view_mode( item.catdatarec.u.d.dirUsrInfo.frView );
							break;
						}
						ret = HFSIFACE_get_dir_next( &item );
					}
					HFSIFACE_get_dir_flush();
				}
			}
		}

	}

	// Find out max/min scroll bounds
#define OUTOFBOUNDS 10000
	if(finder) {
		int x = OUTOFBOUNDS, y = OUTOFBOUNDS, px, py;
		int is_inited = 0, is_invisible = 0;
		memset( &item, 0, sizeof(item) );
		item.volinx = volinx;
		item.id = id;
		ret = HFSIFACE_get_dir_first_readonly( volpath, path, &item );
		while (ret == 0) {
			if(item.catdatarec.cdrType == cdrDirRec) {
				is_inited = (item.catdatarec.u.d.dirUsrInfo.frFlags & kHasBeenInited) != 0;
				is_invisible = (item.catdatarec.u.d.dirUsrInfo.frFlags & fInvisible);
				px = (int)(signed short)item.catdatarec.u.d.dirUsrInfo.frLocation.h;
				py = (int)(signed short)item.catdatarec.u.d.dirUsrInfo.frLocation.v;
				if(px == -1 && py == -1) is_inited = 0;
				if(is_invisible == 0 || m_mac_show_invisibles) {
					if(is_inited || is_inside_magic(px,py)) {
						px = unmagic(px);
						py = unmagic(py);
						if(px < x) x = px;
						if(py < y) y = py;
					}
				}
			} else if(item.catdatarec.cdrType == cdrFilRec) {
				is_inited = (item.catdatarec.u.f.filUsrWds.fdFlags & kHasBeenInited) != 0;
				is_invisible = (item.catdatarec.u.f.filUsrWds.fdFlags & fInvisible);
				if(strcmp((char *)item.catkeyrec.ckrCName,"Icon\x00D") == 0) is_invisible = 1;
				px = (int)(signed short)item.catdatarec.u.f.filUsrWds.fdLocation.h;
				py = (int)(signed short)item.catdatarec.u.f.filUsrWds.fdLocation.v;
				if(px == -1 && py == -1) is_inited = 0;
				if(is_invisible == 0 || m_mac_show_invisibles) {
					if(is_inited || is_inside_magic(px,py)) {
						px = unmagic(px);
						py = unmagic(py);
						if(px < x) x = px;
						if(py < y) y = py;
					}
				}
			}
			ret = HFSIFACE_get_dir_next( &item );
		}
		if(x == OUTOFBOUNDS) x = 0;
		if(y == OUTOFBOUNDS) y = 0;
		snapshot.originX=x;snapshot.originY=y;
	}
#undef OUTOFBOUNDS

	// enumerate items into list view
	memset( &item, 0, sizeof(item) );
	item.volinx = volinx;
	item.id = id;

	errno=0;ret = HFSIFACE_get_dir_first_readonly( volpath, path, &item );
 if(ret<0)throw std::runtime_error(hfs_error?hfs_error:"Cannot read classic HFS directory");
	while (ret == 0) {
		mac_to_pc_charset( item.catkeyrec.ckrCName );
		BrowserDirectory::ObserveRead((char*)item.catkeyrec.ckrCName);
		BrowserDirectory::Row row;if(prepare_classic_browser_row(&item,finder_view_mode,row)){row.path=directory+(directory.empty()||directory.back()!=':'?":":"")+(char*)item.catkeyrec.ckrCName;snapshot.rows.push_back(std::move(row));}
		ret = HFSIFACE_get_dir_next( &item );
	}

 if(errno!=ENOENT)throw std::runtime_error(hfs_error?hfs_error:"Classic HFS directory enumeration failed");
 snapshot.finderMode=finder_view_mode;
	return snapshot;
}
