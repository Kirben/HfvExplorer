// #define ZAPPER

/*
 * HFVExplorer
 * Copyright (C) 1997-1998 by Anygraaf Oy
 * Author: Lauri Pesonen, email: lpesonen@clinet.fi or lauri.pesonen@anygraaf.fi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

//#include "afx.h"
#include "stdafx.h"
#include "LegacyEngine.h"
#include "ClassicFormatCommand.h"
#include "BackgroundCopies.h"
#include "BrowserLifetime.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "BrowserDirectory.h"
#include "ClassicCatalog.h"
#include "BrowserPresentation.h"
#include "MediaDiscovery.h"
#include "StorageJobDialog.h"
#include "HostFileSizes.h"
#include "HFVExplorer.h"

#include "HFVExplorerDoc.h"
#include "HFVExplorerListView.h"
#include "CFATVolume.h"
#include "utils.h"
#include "hfs\ClassicHfs.h"
#include "hfs\interface.h"
#include "AskNewVolume.h"
#include "MainFrm.h"
#include "floppy.h"
#include "adouble.h"
#include "aspecial.h"

#ifdef _DEBUG
// #define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerDoc

IMPLEMENT_DYNCREATE(CHFVExplorerDoc, CDocument)

BEGIN_MESSAGE_MAP(CHFVExplorerDoc, CDocument)
	//{{AFX_MSG_MAP(CHFVExplorerDoc)
	ON_COMMAND(ID_VIEW_REFRESH, OnViewRefresh)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerDoc construction/destruction

void dbdb(char *s)
{
	OutputDebugString( s );
	OutputDebugString( "\r\n" );
}

void CHFVExplorerDoc::set_floppy_volume_dirty( char *path )
{
	int inx, drive;
	char name[_MAX_PATH];

	for(drive=0; drive<MAX_FLOPPIES; drive++) {
		if(is_floppy_by_index(drive)) {
			get_floppy_volume_file_name( drive, name );
			if(stricmp( name, path ) == 0) {
				inx = Mounts().FindClassicSlot( CString(path) );
				if(inx >= 0) {
					Mounts().MarkClassicDirty(inx);
				}
				break;
			}
		}
	}

}

int check_zero_size( char *lpszPathName )
{
	int ret = 1;
	DWORD sizelo, sizehi;
	HANDLE hFile;
	char vname[_MAX_PATH];
	hfv_size_t file_size; int init;

	if(lpszPathName && *lpszPathName) {
		hFile = CreateFile(
			lpszPathName,
			GENERIC_READ,
			0, // not shared now
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL );
		if(hFile != INVALID_HANDLE_VALUE) {
			sizelo = GetFileSize( hFile, &sizehi );
			CloseHandle( hFile );
			if(sizehi == 0 && sizelo == 0) {
				*vname = 0;
				if(ask_new_volume( lpszPathName, vname, &file_size, &init )) {
					try {
						FormatClassicImage(VolumeCoord::Wide(lpszPathName),vname,file_size);
					} catch(const std::exception& error) {
						AfxMessageBox(error.what(),MB_OK|MB_ICONERROR);
						ret = 0;
					}
				} else {
					ret = 0;
				}
			}
		}
	}
	return(ret);
}

int CHFVExplorerDoc::insert_volume(CString name,int is_floppy,int is_cd,int is_hd,int is_removable)
{
    const bool usable=check_zero_size(name.GetBuffer(_MAX_PATH))!=0;
    name.ReleaseBuffer();
    if(!usable)return -1;
    if(Mounts().FindClassicSlot(name)>=0)return -1;
    BrowserMountInventory::ClassicMount volume;
    volume.m_file_name=name;
    volume.m_is_floppy=is_floppy!=0;
    volume.m_is_cd=is_cd!=0;
    volume.m_is_hd=is_hd!=0;
    volume.m_is_removable=is_removable!=0;
    return GetBrowserSession().Mounts().AddClassic(std::move(volume));
}

void CHFVExplorerDoc::enum_volumes_ext( LPSTR mask, LPSTR ext )
{
	HANDLE fh;
	WIN32_FIND_DATA FindFileData;
	int ok;

	fh = FindFirstFile( CString(mask) + "\\*." + ext, &FindFileData );
	ok = fh != INVALID_HANDLE_VALUE;
	while(ok) {
		if(*FindFileData.cFileName != '%') { // skip resource fork files
			(void)insert_volume( CString(mask) + "\\" + CString(FindFileData.cFileName), 0, 0, 0, 0 );
		}
		ok = FindNextFile( fh, &FindFileData );
	}
	if(fh != INVALID_HANDLE_VALUE) FindClose( fh );
}

void CHFVExplorerDoc::enum_hfs_volumes(LPSTR mask)
{
	// I don't want to say *.HF* here ...

	enum_volumes_ext( mask, "HFV" );
	enum_volumes_ext( mask, "DSK" );
	enum_volumes_ext( mask, "HF0" );
	enum_volumes_ext( mask, "HF1" );
	enum_volumes_ext( mask, "HF2" );
	enum_volumes_ext( mask, "HF3" );
	enum_volumes_ext( mask, "HF4" );
	enum_volumes_ext( mask, "HF5" );
	enum_volumes_ext( mask, "HF6" );
	enum_volumes_ext( mask, "HF7" );
	enum_volumes_ext( mask, "HF8" );
	enum_volumes_ext( mask, "HF9" );
}

HICON get_dos_icon( CHFVExplorerApp *pApp, char *p, int iconMode )
{
	char executable[MAX_PATH];
	HICON hicon;

	// pApp = (CHFVExplorerApp *)AfxGetApp();

	if(iconMode == DI_NO_ICONS) return(0);

	if(iconMode == DI_PREFER_INDIVIDUAL_ICONS) {
		hicon = ::ExtractIcon( pApp->m_hInstance, p, 0 );
		if(hicon && hicon != (HICON)1) {
			// dump_icon( hicon );
			return(hicon);
		}
	}
	if(is_extension( p, ".EXE" )) {
		hicon = ::ExtractIcon( pApp->m_hInstance, p, 0 );
		return( hicon );
	}
	if( (UINT_PTR)FindExecutable( p, "", executable ) > 32) {
		hicon = ::ExtractIcon( pApp->m_hInstance, executable, 0 );
		if(hicon && hicon != (HICON)1) return( hicon );
	}
	if(iconMode == DI_PREFER_INDIVIDUAL_ICONS) {
		// we already know that there is no icon
		return(0);
	}
	hicon = ::ExtractIcon( pApp->m_hInstance, p, 0 );
	if(hicon == (HICON)1) hicon = 0;
	return( hicon );
}

#pragma optimize("g",off)
/*
void CHFVExplorerDoc::finder_dat_walk( int volinx, LPSTR path, int list )
{
	HANDLE fh;
	WIN32_FIND_DATA FindFileData;
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString mask;
	unsigned char *finderdat = 0;

	mask = CString(path);
	if(mask.Right(1) != "\\") mask += "\\";

	fh = FindFirstFile( mask + "FINDER.DAT", &FindFileData );
	if(fh == INVALID_HANDLE_VALUE) return(0);
	FindClose( fh );
	finderdat = 0;
	return(1);
}
*/

int is_afp_directory( char *path )
{
	int len, result = 0;
	char _path[_MAX_PATH];
	HANDLE hStream;
	strcpy( _path, path );
	len = strlen( _path );
	if( len > 3 && _path[len-1] == '\\' ) _path[len-1] = 0;
	strcat( _path, ":AFP_AfpInfo" );
	hStream = CreateFile( _path,
												GENERIC_READ,
												FILE_SHARE_WRITE | FILE_SHARE_READ,
												NULL,
												OPEN_EXISTING,
												0,
												NULL );
	if( hStream != INVALID_HANDLE_VALUE ) {
		result = 1;
		CloseHandle(hStream);
	}
	return(result);
}

#ifdef HFV_TESTING
void CHFVExplorerDoc::open_fat_directory(int index, LPSTR path, int list)
{
    auto& session=GetBrowserSession();
    const std::string directory=path;
    if(list){
        session.ClearCurrent();
        auto parent=directory;
        const auto separator=parent.find_last_of('\\');
        const bool root=is_fat_root_directory(path)!=0;
        if(root||separator==parent.npos)parent.clear();
        else parent.resize(separator==2?3:separator);
        session.PrepareFolderPresentation(session.Tree()->GetTreeCtrl().GetSelectedItem(),root?std::nullopt:std::optional<std::wstring>(VolumeCoord::Wide(parent)),false);
    }
    BrowserDirectory::Snapshot snapshot;
    folderReader_->SetOptions(FolderOptions());
    const auto result=ShowStorageWaitJob([&](Storage::Job& job){
        snapshot=folderReader_->read_windows_browser_directory(VolumeCoord::Wide(directory),list!=0,job);
        return Storage::Result{};
    },true,GetBrowserSession().Owner());
    if(result.code){
        session.List()->SetBrowserCapabilities({});
        return;
    }
    PublishWindowsFolder(index,snapshot,session.Tree()->GetTreeCtrl().GetSelectedItem());
}
#endif

#pragma optimize("",on)

int CHFVExplorerDoc::insert_fat_volume(char letter,UINT type)
{
    return GetBrowserSession().Mounts().AddWindows(letter,type);
}

void CHFVExplorerDoc::enum_fat_volumes()
{
	UINT type;
	char rootdir[20], letter;

	for( letter = 'A'; letter <= 'Z'; letter++ ) {
		wsprintf( rootdir, "%c:\\", letter );
		type = GetDriveType( rootdir );
		switch( type ) {
			case DRIVE_REMOVABLE:
			case DRIVE_FIXED:
			case DRIVE_REMOTE:
			case DRIVE_CDROM:
			case DRIVE_RAMDISK:
				insert_fat_volume( letter, type );
				break;
		}
	}
}

BrowserFolderReader::Options CHFVExplorerDoc::FolderOptions() const
{
 const auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 return {m_pc_show_invisibles!=0,m_mac_show_invisibles!=0,m_bits_per_pixel,
         app.m_dosicons,app.m_fat_icon_time_limit};
}

void CHFVExplorerDoc::update_invisible_attributes()
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	m_mac_show_invisibles = pApp->m_show_invisible_mac;
	m_pc_show_invisibles = pApp->m_show_invisible_fat;
}

CHFVExplorerDoc::CHFVExplorerDoc(bool discover)
 :m_browserSession(new BrowserSession(this))
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	// Pane/frame creation registers the legacy single-frame alias.

	// m_mac_show_invisibles = 1;
	// m_pc_show_invisibles = 1;
	update_invisible_attributes();



	if(discover){
	char dir[_MAX_PATH+1];
	char *cuthere;
	::GetModuleFileName( pApp->m_hInstance, dir, _MAX_PATH );
	cuthere = strrchr( dir, '\\' );
	if(cuthere) *cuthere = 0;
	enum_hfs_volumes(dir);

	enum_fat_volumes();
 }

	HDC dc = GetDC(0);
	if(dc) {
		m_bits_per_pixel = GetDeviceCaps( dc, BITSPIXEL );
		ReleaseDC( 0, dc );
	} else {
		m_bits_per_pixel = 1;
	}
#ifdef HFV_TESTING
    folderReader_=std::make_unique<BrowserFolderReader>(FolderOptions());
#endif
}

CHFVExplorerDoc::~CHFVExplorerDoc()
{
#ifdef HFV_TESTING
 folderReader_.reset();
#endif
 m_browserSession.reset();
}

BOOL CHFVExplorerDoc::OnNewDocument()
{
	// if (!CDocument::OnNewDocument()) return FALSE;
	return ( OnOpenDocument("") );
}

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerDoc serialization

void CHFVExplorerDoc::Serialize(CArchive& ar)
{
	if (ar.IsStoring())
	{
	}
	else
	{
	}
}

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerDoc diagnostics

#ifdef _DEBUG
void CHFVExplorerDoc::AssertValid() const
{
	CDocument::AssertValid();
}

void CHFVExplorerDoc::Dump(CDumpContext& dc) const
{
	CDocument::Dump(dc);
}
#endif //_DEBUG

// Catalog lookup cursor belongs to this document/session.

void CHFVExplorerDoc::set_current_hitem( HTREEITEM hItem )
{
	CHFVExplorerApp	*pApp;

	if(hItem) {
		pApp = (CHFVExplorerApp *)AfxGetApp();
		GetBrowserSession().Tree()->GetTreeCtrl().EnsureVisible( hItem );
		GetBrowserSession().Tree()->GetTreeCtrl().SelectItem( hItem );
	}
}

HTREEITEM CHFVExplorerDoc::get_current_hitem()
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	return( GetBrowserSession().Tree()->GetTreeCtrl().GetSelectedItem() );
}

/*
void CHFVExplorerDoc::open_parent_directory(unsigned long id)
{
	HTREEITEM hItem;
	int volinx;

	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	hItem = GetBrowserSession().Tree()->GetTreeCtrl().GetSelectedItem();
	if(hItem) {
		if( id == ID_GOTO_PARENT ) {
			hItem = GetBrowserSession().Tree()->GetTreeCtrl().GetParentItem( hItem );
			if(hItem) {
				GetBrowserSession().Tree()->GetTreeCtrl().SelectItem( hItem );
			}
		} else {
			DWORD_PTR dat;
			dat = GetBrowserSession().Tree()->GetTreeCtrl().GetItemData( hItem );
			volinx = GetBrowserSession().Tree()->get_lparam_volinx(dat);
			if(GetBrowserSession().Tree()->get_lparam_type(dat) == TREE_FAT) {
				hItem = GetBrowserSession().List()->get_lparam_htree(dat);
				if(hItem) {
					GetBrowserSession().Tree()->GetTreeCtrl().SelectItem( hItem );
					// open_fat_directory( volinx, dir );
				}
			} else {
				GetBrowserSession().Tree()->select_by_id( GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,volinx}), volinx, id );
				open_sub_directory( volinx, id, "" );
			}
		}
	}
}
*/

CString CHFVExplorerDoc::get_directory (
	HTREEITEM hItem, 
	int *pmac )
{
	;
	CHFVExplorerApp	*pApp;
	unsigned long id;
	int volinx;

	*pmac = 0;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	if(!hItem) hItem = GetBrowserSession().Tree()->GetTreeCtrl().GetSelectedItem();
	if(hItem) {
		DWORD_PTR dat;
		dat = GetBrowserSession().Tree()->GetTreeCtrl().GetItemData( hItem );
		switch(GetBrowserSession().Tree()->get_lparam_type(dat)) {
			case LIST_UNKNOWN:
			case LIST_SPEC:
				return("");
			case LIST_HFS:
				*pmac = 1;
				volinx = GetBrowserSession().Tree()->get_lparam_volinx(dat);
				id = GetBrowserSession().Tree()->get_lparam_id(dat);
				return( GetBrowserSession().Tree()->get_directory_name(
					GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,volinx}),
					volinx, id ) );
				break;
			case LIST_FAT:
				char path[MAX_PATH];
				if(GetBrowserSession().Tree()->get_lparam_path(dat,path,MAX_PATH)) {
					return(CString(path));
				}
				break;
		}
	}
	return("");
}

#ifdef HFV_TESTING
void CHFVExplorerDoc::open_sub_directory(int index,unsigned long id,CString name)
{
    auto& session=GetBrowserSession();
    auto& volume=Mounts().ClassicAt(index);
    auto tree=session.Tree();
    const auto root=session.MountRoot({BrowserMountInventory::Kind::ClassicVolume,index});
    auto path=name.IsEmpty()?tree->get_directory_name(root,index,id):name;
    path+=":";
    const auto parent=tree->get_parent_id(root,index,id);
    const auto finder=session.finderLayout;
    BrowserDirectory::Snapshot snapshot;
    session.ClearCurrent();
    const auto capture=Mounts().Lookup({BrowserMountInventory::Kind::ClassicVolume,index});
    if(!capture)return;
    folderReader_->SetOptions(FolderOptions());
    folderReader_->ClassicState(index)=volume;
    const auto result=ShowStorageWaitJob([&](Storage::Job& job){
        snapshot=folderReader_->read_classic_browser_directory(index,id,path,finder,parent,&job);
        return Storage::Result{};
    },true,session.Owner());
    if(result.code||!Mounts().PublishClassic(*capture,folderReader_->ClassicState(index))){
        session.List()->initialize_volume("",0);
        session.List()->SetBrowserCapabilities({});
        return;
    }
    if(id==CNID_ROOT_ID){
        path=volume.m_volume_name+":";
        tree->GetTreeCtrl().SetItemText(root,volume.m_volume_name);
    }
    const auto item=tree->find_item(index,root,id);
    const auto label=parent?std::optional<std::wstring>(VolumeCoord::Wide((LPCSTR)tree->get_directory_name(root,index,parent))):std::nullopt;
    PublishClassicFolder(index,item,label,snapshot);
}
#endif

void CHFVExplorerDoc::PublishClassicFolder(int volinx,HTREEITEM item,const std::optional<std::wstring>& parent,
    const BrowserDirectory::Snapshot& snapshot)
{
 int finder_view_mode=snapshot.finderMode;
 if(GetBrowserSession().finderLayout){
		switch(finder_view_mode) {
			case FINDER_VIEW_MODE_ICON:
			case FINDER_VIEW_MODE_ICON_BUTTON:
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_ICON) {
					GetBrowserSession().List()->set_style( LVS_ICON );
				}
				break;
			case FINDER_VIEW_MODE_ICON_SMALL:
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_SMALLICON) {
					GetBrowserSession().List()->set_style( LVS_SMALLICON );
				}
				break;
			case FINDER_VIEW_MODE_ICON_NAME:
				GetBrowserSession().List()->m_sorted_by_column = FIELD_NAME;
				GetBrowserSession().List()->m_sort_is_ascending = 1;
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) {
					GetBrowserSession().List()->set_style( LVS_REPORT );
				}
				break;
			case FINDER_VIEW_MODE_ICON_DATE:
				GetBrowserSession().List()->m_sorted_by_column = FIELD_MODIFIED;
				GetBrowserSession().List()->m_sort_is_ascending = 1;
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) {
					GetBrowserSession().List()->set_style( LVS_REPORT );
				}
				break;
			case FINDER_VIEW_MODE_ICON_SIZE:
				GetBrowserSession().List()->m_sorted_by_column = FIELD_SIZE;
				GetBrowserSession().List()->m_sort_is_ascending = 1;
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) {
					GetBrowserSession().List()->set_style( LVS_REPORT );
				}
				break;
			case FINDER_VIEW_MODE_ICON_KIND:
				GetBrowserSession().List()->m_sorted_by_column = FIELD_TYPE;
				GetBrowserSession().List()->m_sort_is_ascending = 1;
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) {
					GetBrowserSession().List()->set_style( LVS_REPORT );
				}
				break;
			case FINDER_VIEW_MODE_ICON_LABEL:
				GetBrowserSession().List()->m_sorted_by_column = FIELD_CREATOR;
				GetBrowserSession().List()->m_sort_is_ascending = 1;
				if((GetWindowLong(GetBrowserSession().List()->m_cl->m_hWnd,GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT) {
					GetBrowserSession().List()->set_style( LVS_REPORT );
				}
				break;
		}
  GetBrowserSession().List()->set_origo(snapshot.originX,snapshot.originY);
 }
 auto& session=GetBrowserSession();
 const auto& path=snapshot.folder.ClassicPath();
 auto title=VolumeCoord::Wide(path);
 if(!title.empty()&&title.back()==L':')title.pop_back();
 CString status;
 status.Format("Exploring \"%s\": %I64u kB free",path.c_str(),Mounts().ClassicAt(volinx).freeBytes/1024);
 const auto root=GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,volinx});
 session.PublishFolder(snapshot,item,parent,std::move(title),status,[&](const BrowserDirectory::Row& row){
    CStringA name(MacBrowser::Wide(row.entry.name).c_str());
    return session.Tree()->tree_insert(root,volinx,const_cast<char*>(name.GetString()),row.entry.id,row.entry.parent);
 });
}



#pragma optimize("",on)

void CHFVExplorerDoc::BuildBrowserRoots()
{
    auto tree=GetBrowserSession().Tree();
    tree->initialize_tree();
    for(int index=0;index<Mounts().ClassicCount();++index){
        auto& volume=Mounts().ClassicAt(index);
        const auto address=BrowserMountInventory::Address{BrowserMountInventory::Kind::ClassicVolume,index};
        GetBrowserSession().SetMountRoot(address,nullptr);
        if(volume.m_free)continue;
        CString label=volume.m_volume_name.IsEmpty()?volume.m_file_name:volume.m_volume_name;
        GetBrowserSession().SetMountRoot(address,tree->initialize_volume(index,label,CNID_ROOT_ID,volume.m_is_floppy,
            volume.m_is_cd,volume.m_is_hd,volume.m_is_removable));
    }
    for(int index=0;index<Mounts().WindowsCount();++index){
        const auto& drive=Mounts().WindowsAt(index);
        GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::WindowsDrive,index},nullptr);
        if(drive.m_type==DRIVE_NO_ROOT_DIR)continue;
        GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::WindowsDrive,index},
            tree->initialize_fat_volume(index,&drive.m_volume_name,drive.m_type,
                &drive.m_volume_name,toupper((unsigned char)drive.m_letter)<'C'));
    }
}

bool CHFVExplorerDoc::NavigateInitialFolder(int firstClassic)
{
    using namespace BrowserNavigation;
    for(int index=firstClassic;index<Mounts().ClassicCount();++index){
        const auto& volume=Mounts().ClassicAt(index);
        if(volume.m_free||!GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,index}))continue;
        const auto image=std::string((LPCSTR)volume.m_file_name);
        Request request{};
        request.intent=Intent::StartupFolder;
        request.folder=FileOperations::Location::Classic(image,":");
        request.folder.directory=true;
        request.folder.expectedResource=VolumeCoord::ImageKey(image);
        request.resource=request.folder.expectedResource;
        if(volume.m_mediaGenerationKnown)request.folder.BindMediaGeneration(volume.m_mediaGeneration);
        request.tryNextStartupRoot=true;
        if(NavigateClassicFolder(index,CNID_ROOT_ID,false,&request))return true;
    }
    // Starting a browser must not implicitly probe an empty floppy or CD.
    for(int index=0;index<Mounts().WindowsCount();++index)
        if(Mounts().WindowsAt(index).m_type==DRIVE_FIXED&&NavigateStartupDirectory((LPCSTR)Mounts().WindowsAt(index).m_volume_name,false))
            return true;
    return false;
}

bool CHFVExplorerDoc::NavigateStartupDirectory(const std::string& path,bool mac)
{
    using namespace BrowserNavigation;
    if(path.empty()||path.size()>=MAX_PATH)return false;
    Request request{};
    request.intent=Intent::StartupFolder;
    if(mac)return NavigateNamedClassicFolder(path);

    if(path.size()<3||path[1]!=':'||path[2]!='\\')return false;
    for(int index=0;index<Mounts().WindowsCount();++index)
        if(toupper((unsigned char)Mounts().WindowsAt(index).m_letter)==toupper((unsigned char)path[0])&&GetBrowserSession().MountRoot({BrowserMountInventory::Kind::WindowsDrive,index})){
            request.folder=FileOperations::Location::Windows(VolumeCoord::Wide(path));
            request.folder.directory=true;
            return NavigateWindowsFolder(index,path,false,&request);
        }
    return false;
}

bool CHFVExplorerDoc::NavigateNamedClassicFolder(const std::string& path,int first,int preferred)
{
    using namespace BrowserNavigation;
    const auto volumeName=path.substr(0,path.find(':'));
    const auto queue=[&](int index,bool tryNext){
        const auto& volume=Mounts().ClassicAt(index);
        if(volume.m_free||!GetBrowserSession().MountRoot({BrowserMountInventory::Kind::ClassicVolume,index}))return false;
        auto image=std::string((LPCSTR)volume.m_file_name);
        Request request{};
        request.intent=Intent::StartupFolder;
        request.folder=FileOperations::Location::Classic(image,path.find(':')==path.npos?path+":":path);
        request.folder.directory=true;
        request.folder.expectedResource=VolumeCoord::ImageKey(image);
        request.resource=request.folder.expectedResource;
        if(volume.m_mediaGenerationKnown)request.folder.BindMediaGeneration(volume.m_mediaGeneration);
        request.tryNextStartupRoot=tryNext;
        return NavigateClassicFolder(index,0,false,&request);
    };
    if(preferred>=0&&preferred<Mounts().ClassicCount()&&Mounts().ClassicAt(preferred).m_volume_name.CompareNoCase(volumeName.c_str())==0)
        return queue(preferred,false);
    for(int index=first;index<Mounts().ClassicCount();++index){
        const auto& volume=Mounts().ClassicAt(index);
        if(volume.m_volume_name.IsEmpty()||volume.m_volume_name.CompareNoCase(volumeName.c_str())==0)
            if(queue(index,true))return true;
    }
    return false;
}

BOOL CHFVExplorerDoc::OnOpenDocument(LPCTSTR path)
{
    auto& session=GetBrowserSession();
    const bool first=!browserOpened_;
    const bool explicitImage=path&&*path;
    if(!first&&explicitImage)return session.MacBrowser().OpenImage(path)>0;

    const auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
    browserOpened_=true;
    if(!first){
        session.ClearCurrent();
        OnCleanDocument();
    }
    session.Tree()->enable_selchange(0);
    BuildBrowserRoots();
    session.Tree()->enable_selchange(1);
    const bool restoring=session.MacBrowser().RebuildTree();
    if(app->m_startup_hds)session.MacBrowser().ScanRemovable(true,true);
    session.MacBrowser().ScanStartupDrives(app->m_startup_floppies!=FALSE,app->m_startup_cds!=FALSE);
    if(explicitImage)return session.MacBrowser().OpenImage(path)>0;
    if(!restoring&&app->m_opendir.IsEmpty())NavigateInitialFolder();
    return TRUE;
}

void CHFVExplorerDoc::my_chdir(CString path,int mac,int preferredVolume)
{
    if(path.IsEmpty()||!GetBrowserSession().Tree())return;
    if(mac)NavigateNamedClassicFolder((LPCSTR)path,0,preferredVolume);
    else NavigateStartupDirectory((LPCSTR)path,false);
}

void CHFVExplorerDoc::OnCleanDocument()
{
 GetBrowserSession().CancelClassicRefresh();
 GetBrowserSession().MacBrowser().DetachTree();
	CHFVExplorerApp	*pApp;

	pApp = (CHFVExplorerApp *)AfxGetApp();
	if(GetBrowserSession().List())GetBrowserSession().List()->list_clear();

	if(GetBrowserSession().Tree()) {
		GetBrowserSession().Tree()->enable_selchange(0);
		GetBrowserSession().Tree()->tree_clear();
	}
	Mounts().CloseClassicVolumes();
}

void CHFVExplorerDoc::FinishBackgroundClose(){
 Mounts().CloseClassicVolumes();
 GetBrowserSession().MacBrowser().Clear();
}
void CHFVExplorerDoc::OnCloseDocument()
{
 bool physical=false;for(int i=0;i<Mounts().ClassicCount();++i)if(Mounts().ClassicAt(i).m_opened&&(Mounts().ClassicAt(i).m_is_cd||Mounts().ClassicAt(i).m_is_floppy)){physical=true;break;}
 if(physical&&BackgroundCopies::Active()){
  // A device reader's final release can wait behind another thread's slow
  // read. Destroy the UI now, retaining only this viewless document for cleanup.
  GetBrowserSession().StopNotifications();m_bAutoDelete=FALSE;
  CDocument::OnCloseDocument();GetBrowserSession().DetachPanes();
  BrowserLifetime::Retire(this);return;
 }
 BrowserLifetime::Collect();
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	if(GetBrowserSession().Tree()) {
		GetBrowserSession().Tree()->enable_selchange(0);
		GetBrowserSession().Tree()->tree_delete_images();
	}
	OnCleanDocument();
 GetBrowserSession().MacBrowser().Clear();
	CDocument::OnCloseDocument();
}

void CHFVExplorerDoc::OnViewRefresh()
{
 auto& session=GetBrowserSession();
 if(!session.RefreshSelected())session.MacBrowser().ScanMedia();
}
