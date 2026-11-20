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

#include "afx.h"
#include "stdafx.h"
#include "VolumeCoordinator.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "DeviceWait.h"
#include "HFVExplorer.h"
#include "HFVExplorerTreeView.h"
#include "HFVExplorerDoc.h"
#include "HFVPreviewView.h"
#include "special.h"
#include "own_ids.h"
#include "MainFrm.h"
#include "utils.h"

extern "C" {
#include "floppy.h"
}

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// HFVExplorerTreeView

IMPLEMENT_DYNCREATE(HFVExplorerTreeView, CTreeView)

#define LISTICONCOUNT 11

void HFVExplorerTreeView::set_style( long style )
{
	long lStyleOld;

	lStyleOld = GetWindowLong(m_ct->m_hWnd, GWL_STYLE);

	lStyleOld &= ~TVS_DISABLEDRAGDROP;
	lStyleOld &= ~TVS_SHOWSELALWAYS;
	lStyleOld &= ~TVS_EDITLABELS;

	lStyleOld |= TVS_HASLINES;
	lStyleOld |= TVS_HASBUTTONS;
	lStyleOld |= TVS_LINESATROOT;
	
	lStyleOld |= style;

	SetWindowLong(m_ct->m_hWnd, GWL_STYLE, lStyleOld);
}


void HFVExplorerTreeView::ApplyDpi(UINT dpi)
{
    auto& app = *static_cast<CHFVExplorerApp*>(AfxGetApp());
    const auto top = m_ct->GetFirstVisibleItem();
    m_icons.Apply(dpi, [&](HIMAGELIST images) {
        m_ct->SendMessage(TVM_SETIMAGELIST,TVSIL_NORMAL,LPARAM(images));
    });
    m_ct->SendMessage(WM_SETFONT, WPARAM(app.get_font(dpi)), TRUE);
    if (top && top != m_ct->GetFirstVisibleItem()) m_ct->SelectSetFirstVisible(top);
}

void HFVExplorerTreeView::Initialize()
{

	tree_clear();
	m_ct->SendMessage(TVM_SETIMAGELIST,TVSIL_NORMAL,LPARAM(m_icons.Handle()));
}

int BrowserTreeNode::LegacyType() const
{
    using Kind = BrowserMountInventory::Kind;
    switch (mount.kind) {
    case Kind::WindowsDrive: return LIST_FAT;
    case Kind::ClassicVolume: return LIST_HFS;
    case Kind::MountedVolume: return LIST_MAC;
    }
    return LIST_UNKNOWN;
}

uint32_t BrowserTreeNode::CatalogId() const
{
    if (const auto* catalog = std::get_if<ClassicTreeFolder>(&value))
        return catalog->catalogId;
    const auto& location = std::get<BrowserDirectory::Folder>(value).location;
    return location.IsMounted() ? location.Id() : location.expectedCatalogId;
}

LPARAM HFVExplorerTreeView::StoreBrowserNode(BrowserTreeNode node)
{
    node.mount=GetBrowserSession().Mounts().Bind(node.mount);
    if (auto* folder = std::get_if<BrowserDirectory::Folder>(&node.value)) folder->location.directory = true;
    auto* stored = new BrowserTreeNode(std::move(node));
    const auto key = reinterpret_cast<DWORD_PTR>(stored);
    INT_PTR slot;
    if (m_free_slots.empty()) {
        slot = m_pointer_array.GetSize();
        m_pointer_array.Add(key);
    } else {
        slot = m_free_slots.back();
        m_free_slots.pop_back();
        m_pointer_array.SetAt(slot, key);
    }
    m_pointer_slots[key] = slot;
    return LPARAM(key);
}

const BrowserTreeNode* HFVExplorerTreeView::BrowserNode(DWORD_PTR data) const
{
    if(!m_pointer_slots.count(data))return nullptr;
    const auto* node=reinterpret_cast<const BrowserTreeNode*>(data);
    return GetBrowserSession().Mounts().IsBound(node->mount)?node:nullptr;
}

const BrowserTreeNode* HFVExplorerTreeView::BrowserNodeAt(HTREEITEM item) const
{
    return item ? BrowserNode(m_ct->GetItemData(item)) : nullptr;
}

int HFVExplorerTreeView::get_lparam_volinx(LPARAM data)
{
    const auto* node = BrowserNode(data);
    return node ? node->mount.index : -1;
}

int HFVExplorerTreeView::get_lparam_id(LPARAM data)
{
    const auto* node = BrowserNode(data);
    return node ? int(node->CatalogId()) : 0;
}

int HFVExplorerTreeView::get_lparam_type(LPARAM data)
{
    const auto* node = BrowserNode(data);
    return node ? node->LegacyType() : LIST_UNKNOWN;
}

int HFVExplorerTreeView::get_htree_type( HTREEITEM htree )
{
	DWORD_PTR dat = m_ct->GetItemData( htree );
	if(!dat) return(LIST_UNKNOWN);
	return(get_lparam_type( dat ));
}

int HFVExplorerTreeView::get_lparam_path(LPARAM data, LPSTR path, int capacity)
{
    if (!path || capacity <= 0) return 0;
    *path = 0;
    const auto* node = BrowserNode(data);
    const auto* folder = node ? node->Folder() : nullptr;
    if (!folder || !folder->location.IsWindows()) return 0;
    BOOL substituted = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, folder->location.WindowsPath().c_str(), -1,
                            path, capacity, nullptr, &substituted) && !substituted) return 1;
    *path = 0;
    return 0;
}

void HFVExplorerTreeView::destroy_pointers()
{
	int i, count = m_pointer_array.GetSize();
	DWORD_PTR dw;
	BrowserTreeNode *p;

	for(i=0; i<count; i++) {
		dw = m_pointer_array.GetAt( i );
		if(dw) {
			p = (BrowserTreeNode *)dw;
			delete p;
		}
	}
	m_pointer_array.RemoveAll();
 m_pointer_slots.clear();m_free_slots.clear();
}

HTREEITEM HFVExplorerTreeView::tree_insert( 
	HTREEITEM root_item,
	int volinx,
	LPSTR name, 
	unsigned long id,
	unsigned long parent_id )
{
	HTREEITEM hItem, h;
	TV_INSERTSTRUCT ins;

	h = find_item( volinx, root_item, id );
	
	if( h ) return(h);
	h = find_item( volinx, root_item, parent_id );

	// if( !h ) h = m_root_item;

	ins.hParent = h;
	// ins.hInsertAfter = TVI_SORT;
	ins.hInsertAfter = TVI_LAST;
	ins.item.iImage = 0;
	ins.item.iSelectedImage = 0;
	ins.item.pszText = name;
	ins.item.lParam = StoreBrowserNode({{BrowserMountInventory::Kind::ClassicVolume, volinx}, ClassicTreeFolder{uint32_t(id)}});
	// ins.item.state = TVIS_EXPANDED;
	ins.item.state = 0;
	ins.item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_TEXT | TVIF_PARAM;
	hItem = m_ct->InsertItem( &ins );

	// m_ct->SetItemState( h, TVIS_EXPANDED, TVIS_EXPANDED );
	// m_ct->SelectItem( h );

	return(hItem);
}

HTREEITEM HFVExplorerTreeView::FindWindowsFolder(HTREEITEM root,const std::wstring& path)
{
 if(!root)return nullptr;
 const auto* folder=BrowserFolderAt(root);
 if(!folder||!folder->location.IsWindows())return nullptr;
 auto existing=folder->location.WindowsPath();
 while(existing.size()>3&&existing.back()==L'\\')existing.pop_back();
 if(!_wcsicmp(path.c_str(),existing.c_str()))return root;
 if(existing.empty()||existing.size()>=path.size()||_wcsnicmp(path.c_str(),existing.c_str(),existing.size())||
    (existing.back()!=L'\\'&&path[existing.size()]!=L'\\'))return nullptr;
 for(auto child=m_ct->GetChildItem(root);child;child=m_ct->GetNextSiblingItem(child))
  if(auto found=FindWindowsFolder(child,path))return found;
 return nullptr;
}

HTREEITEM HFVExplorerTreeView::InsertWindowsFolder(HTREEITEM parent,int volume,const std::wstring& name,
    const FileOperations::Location& location,bool writable)
{
 if(auto found=FindWindowsFolder(parent,location.WindowsPath()))return found;
 TVINSERTSTRUCTW item{};
 item.hParent=parent;
 item.hInsertAfter=TVI_SORT;
 item.item.mask=TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_TEXT|TVIF_PARAM;
 item.item.iImage=item.item.iSelectedImage=11;
 item.item.pszText=const_cast<wchar_t*>(name.c_str());
 item.item.lParam=StoreBrowserNode({{BrowserMountInventory::Kind::WindowsDrive, volume},
                                   BrowserDirectory::Folder{location,writable}});
 return reinterpret_cast<HTREEITEM>(SendMessageW(m_ct->m_hWnd,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item)));
}

void HFVExplorerTreeView::select_by_id( HTREEITEM root_item, int volinx, unsigned long id )
{
	HTREEITEM h;
	if(id) {
		h = find_item( volinx, root_item, id );
		if(h) {
			m_ct->SelectItem( h );
		}
	}
}
void HFVExplorerTreeView::expand_dir( 
	HTREEITEM root_item,
	int volinx, 
	unsigned long parent ) 
{
	HTREEITEM h;
	h = find_item( volinx, root_item, parent );
	if(h) {
		m_ct->Expand( h, TVE_EXPAND );
	}
}

HTREEITEM HFVExplorerTreeView::find_item( 
	int volinx, HTREEITEM root, unsigned long id )
{
	HTREEITEM h, r;
	unsigned long id2;
	int vol2;

	int loopguard = 0;

	if(!root) return(0);
	id2 = get_lparam_id( m_ct->GetItemData( root ) );
	vol2 = get_lparam_volinx( m_ct->GetItemData( root ) );
	if( id2 == id && volinx == vol2 ) return(root);
	if( m_ct->ItemHasChildren(root) ) {
		h = m_ct->GetChildItem( root );
		if(h != 0 && h != (HTREEITEM)(INT_PTR)-1) {
			r = find_item( volinx, h, id );
			if(r) return(r);
			h = m_ct->GetNextSiblingItem( h );
			while(h) {
				r = find_item( volinx, h, id );
				if(r) return(r);
				h = m_ct->GetNextSiblingItem( h );
				if(loopguard++ > 1000) break;
			}
		}
	}
	return(0);
}

// pc version
HTREEITEM HFVExplorerTreeView::tree_find_fat( HTREEITEM root, LPSTR path )
{
	HTREEITEM h, r;
	int len1, len2;
	char p[MAX_PATH];
	DWORD_PTR dat;

	int loopguard = 0;

	if(!root) return(0);
	
	dat = m_ct->GetItemData( root );
	if(!dat) return(0);

	if(!get_lparam_path( dat, p, MAX_PATH-1 )) return(0);
	if( stricmp(path,p) == 0 ) return(root);

	if( m_ct->ItemHasChildren(root) ) {
		len1 = strlen(path);
		len2 = strlen(p);

		if(len2 >= len1) return(0);
		if(strnicmp(p,path,min(len1,len2)) != 0) return(0);

		// now it's ok substring

		h = m_ct->GetChildItem( root );
		r = tree_find_fat( h, path );
		if(r) return(r);
		h = m_ct->GetNextSiblingItem( h );
		while(h) {
			r = tree_find_fat( h, path );
			if(r) return(r);
			h = m_ct->GetNextSiblingItem( h );
			if(loopguard++ > 1000) break;
		}
	}
	return(0);
}

unsigned long HFVExplorerTreeView::get_parent_id( 
	HTREEITEM root_item,
	int volinx, 
	unsigned long id )
{
	HTREEITEM h;
	unsigned long newid = 0;

	h = find_item( volinx, root_item, id );
	if(h) {
		h = m_ct->GetParentItem( h );
		if( h ) {
			newid = get_lparam_id( m_ct->GetItemData( h ) );
		}
	}
	return(newid);
}

CString HFVExplorerTreeView::get_directory_name( 
	HTREEITEM root_item,
	int volinx, 
	unsigned long id )
{
	CString name, sname;
	HTREEITEM h;

	int loopguard = 0;
	
	name = "";
	h = find_item( volinx, root_item, id );
	if(h) {
		while( h ) {
			sname = m_ct->GetItemText( h );
			if( name == "" ) {
				name = sname;
			} else {
				name = sname + ":" + name;
			}
			h = m_ct->GetParentItem( h );
			if(loopguard++ > 5000) break;
		}
	}
	return(name);
}

HTREEITEM HFVExplorerTreeView::initialize_volume( 
	int volinx,
	CString root_name, 
	unsigned long id,
	int is_floppy,
	int is_cd,
	int is_hd,
	int is_removable )
{
	TV_INSERTSTRUCT ins;
	LPSTR r;
	HTREEITEM item;
	
	r = root_name.GetBuffer(MAX_PATH-1);
	ins.hParent = NULL;
	// ins.hInsertAfter = TVI_SORT;
	ins.hInsertAfter = TVI_LAST;
	if(is_floppy) {
		ins.item.iImage = 6;
	} else if(is_cd) {
		ins.item.iImage = 9;
	} else if(is_hd) {
		if(is_removable) {
			ins.item.iImage = 18;
		} else {
			ins.item.iImage = 7;
		}
	} else {
		ins.item.iImage = 3;
	}
	ins.item.iSelectedImage = ins.item.iImage;
	ins.item.pszText = r;
	ins.item.lParam = StoreBrowserNode({{BrowserMountInventory::Kind::ClassicVolume, volinx}, ClassicTreeFolder{uint32_t(id)}});
	ins.item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_TEXT | TVIF_PARAM;
	item = m_ct->InsertItem( &ins );
	root_name.ReleaseBuffer();

	// m_ct->Expand( root_item, TVE_EXPAND );
	// m_ct->SelectItem( root_item );

	return(item);
}

HTREEITEM HFVExplorerTreeView::initialize_fat_volume( 
	int volinx,
	const CString *plabel,
	UINT type,
	const CString *ppath,
	BOOL is_floppy
)
{
	TV_INSERTSTRUCT ins;
	HTREEITEM item;

	ins.hParent = NULL;
	ins.hInsertAfter = TVI_LAST;
	switch( type ) {
		case DRIVE_REMOVABLE: 
			if(is_floppy)
				ins.item.iImage = 13; 
			else
				ins.item.iImage = 19;
			break;
		case DRIVE_FIXED:			
			ins.item.iImage = 14; 
			break;
		case DRIVE_REMOTE:		
			ins.item.iImage = 15; 
			break;
		case DRIVE_CDROM:			
			ins.item.iImage = 16; 
			break;
		case DRIVE_RAMDISK:		
			ins.item.iImage = 17; 
			break;
	}
	
	ins.item.iSelectedImage = ins.item.iImage;
	ins.item.pszText = const_cast<LPSTR>(plabel->GetString());
	auto location = FileOperations::Location::Windows(VolumeCoord::Wide(ppath->GetString()));
    location.directory = true;
    ins.item.lParam = StoreBrowserNode({{BrowserMountInventory::Kind::WindowsDrive, volinx},
                                       BrowserDirectory::Folder{std::move(location), type != DRIVE_CDROM}});
	ins.item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_TEXT | TVIF_PARAM;
	item = m_ct->InsertItem( &ins );
	return(item);
}

int HFVExplorerTreeView::initialize_tree()
{
	tree_clear();
	set_style( 0 );

	m_ct->SendMessage(TVM_SETIMAGELIST,TVSIL_NORMAL,LPARAM(m_icons.Handle()));

	/*
	TV_INSERTSTRUCT ins;
	ins.hParent = NULL;
	ins.hInsertAfter = TVI_SORT;
	ins.item.iImage = 5;
	ins.item.iSelectedImage = 5;
	ins.item.pszText = configurations_name;
	ins.item.lParam = make_lparam( LIST_SPEC, -1, configurations_id, 0 );
	ins.item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_TEXT | TVIF_PARAM;
	//m_control_item = 
			m_ct->InsertItem( &ins );
	*/

	return(1);
}

void HFVExplorerTreeView::tree_delete_images()
{
	// what about icons created?
	m_ct->SetImageList( 0, 0 );
}

void HFVExplorerTreeView::enable_selchange(int onoff)
{
	m_selchange_enabled = onoff;
}

int HFVExplorerTreeView::tree_clear()
{
	m_selchange_enabled = 0;
	mountRoots_.clear();
	m_ct->SetImageList( 0, 0 );
	if(this && IsWindow( this->m_hWnd )) {
		m_ct->DeleteAllItems();
	}
	return(1);
}

HFVExplorerTreeView::HFVExplorerTreeView()
{
	CHFVExplorerApp	*pApp;

	m_ct = &this->GetTreeCtrl();

	m_selchange_enabled = 0;

	pApp = (CHFVExplorerApp *)AfxGetApp();

	::SendMessage( 
		m_ct->m_hWnd, 
		WM_SETFONT,
		(WPARAM)pApp->get_font(),
		MAKELPARAM(TRUE,0) );

    m_icons.Create(LISTICONCOUNT);
	m_icons.Add(pApp->LoadIcon(IDI_ICON1));
	m_icons.Add(pApp->LoadIcon(IDI_ICON2));
	m_icons.Add(pApp->LoadIcon(IDI_ICON3));
	m_icons.Add(pApp->LoadIcon(IDI_ICON4));
	m_icons.Add(pApp->LoadIcon(IDI_ICON5));
	m_icons.Add(pApp->LoadIcon(IDI_ICON6));
	m_icons.Add(pApp->LoadIcon(IDI_ICON51));
	m_icons.Add(pApp->LoadIcon(IDI_ICON52));
	m_icons.Add(pApp->LoadIcon(IDI_ICON53));
	m_icons.Add(pApp->LoadIcon(IDI_ICON54));
	m_icons.Add(pApp->LoadIcon(IDI_ICON55));

	// index==11
	m_icons.Add(pApp->LoadIcon(IDI_ICON1_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON4_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON51_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON52_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON53_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON54_YELLOW));
	m_icons.Add(pApp->LoadIcon(IDI_ICON55_YELLOW));

	// index==18
	m_icons.Add(pApp->LoadIcon(IDI_ICON56));
	// index==19
	m_icons.Add(pApp->LoadIcon(IDI_ICON56_YELLOW));
}

HFVExplorerTreeView::~HFVExplorerTreeView()
{
 if(m_browserSession)m_browserSession->Detach(this);
	destroy_pointers();
	m_selchange_enabled = 0;


	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();

}

BEGIN_MESSAGE_MAP(HFVExplorerTreeView, CTreeView)
	//{{AFX_MSG_MAP(HFVExplorerTreeView)
	ON_NOTIFY_REFLECT(TVN_SELCHANGED, OnSelchanged)
 ON_NOTIFY_REFLECT(TVN_DELETEITEM, OnDeleteItem)
	ON_WM_CHAR()
	ON_WM_CONTEXTMENU()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// HFVExplorerTreeView diagnostics

#ifdef _DEBUG
void HFVExplorerTreeView::AssertValid() const
{
	CTreeView::AssertValid();
}

void HFVExplorerTreeView::Dump(CDumpContext& dc) const
{
	CTreeView::Dump(dc);
}
#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// HFVExplorerTreeView message handlers

HTREEITEM HFVExplorerTreeView::MountRoot(BrowserMountInventory::Address address) const
{
    const auto found=mountRoots_.find({address.kind,address.index});
    return found==mountRoots_.end()?nullptr:found->second;
}

void HFVExplorerTreeView::SetMountRoot(BrowserMountInventory::Address address,HTREEITEM root)
{
    const auto slot=std::make_pair(address.kind,address.index);
    if(root)mountRoots_[slot]=root;
    else mountRoots_.erase(slot);
}

void HFVExplorerTreeView::RenewBrowserMount(HTREEITEM item)
{
    if(!item)return;
    const auto data=m_ct->GetItemData(item);
    if(!m_pointer_slots.count(data))return;
    auto* node=reinterpret_cast<BrowserTreeNode*>(data);
    node->mount=GetBrowserSession().Mounts().Bind(node->mount);
}

void HFVExplorerTreeView::BindBrowserFolder(HTREEITEM item, BrowserDirectory::Folder folder)
{
    const auto* node = BrowserNodeAt(item);
    if (!node || folder.location.IsNone()) return;
    if (folder.location.IsClassic() && !folder.location.expectedCatalogId)
        folder.location.expectedCatalogId = node->CatalogId();
    const_cast<BrowserTreeNode*>(node)->value = std::move(folder);
}

const BrowserDirectory::Folder* HFVExplorerTreeView::BrowserFolderAt(HTREEITEM item) const
{
    const auto* node = BrowserNodeAt(item);
    return node ? node->Folder() : nullptr;
}

void HFVExplorerTreeView::OnDeleteItem(NMHDR* header,LRESULT* result){
 auto item=reinterpret_cast<NMTREEVIEW*>(header);DWORD_PTR key=DWORD_PTR(item->itemOld.lParam);auto found=m_pointer_slots.find(key);
 if(found!=m_pointer_slots.end()){
  auto* node=reinterpret_cast<BrowserTreeNode*>(key);
  if(MountRoot(node->mount)==item->itemOld.hItem)SetMountRoot(node->mount,nullptr);
  const auto slot=found->second;
  m_pointer_array.SetAt(slot,0);
  m_free_slots.push_back(slot);
  m_pointer_slots.erase(found);
  delete node;
 }
 *result=0;
}
void HFVExplorerTreeView::OnSelchanged(NMHDR* pNMHDR, LRESULT* pResult) 
{
 if(GetBrowserSession().List() && GetBrowserSession().List()->IsDragging()){*pResult=0;return;}
 if(StorageUi::busy()){*pResult=0;return;}
    auto& session=GetBrowserSession();
    if(!SelectionNotificationsEnabled()){*pResult=0;return;}
    session.CancelNavigation();

    const auto item=reinterpret_cast<NM_TREEVIEW*>(pNMHDR)->itemNew.hItem;
    const auto* node=BrowserNodeAt(item);
    if(node&&m_pDocument){
        auto& doc=*static_cast<CHFVExplorerDoc*>(m_pDocument);
        enable_selchange(0);
        const auto address=node->mount;
        switch(address.kind){
        case BrowserMountInventory::Kind::MountedVolume:
            session.MacBrowser().Select(m_ct->GetItemData(item));
            break;
        case BrowserMountInventory::Kind::ClassicVolume:
            doc.NavigateClassicFolder(address.index,node->CatalogId());
            break;
        case BrowserMountInventory::Kind::WindowsDrive:
            if(const auto* folder=node->Folder())
                doc.NavigateWindowsFolder(address.index,folder->location.WindowsPath());
            break;
        }
    }

	enable_selchange(1);

	*pResult = 0;
}

void HFVExplorerTreeView::OnChar(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	HTREEITEM hItem;
	int ok = 0;

	if( nChar == VK_RETURN ) {
		hItem = m_ct->GetSelectedItem();
		if( hItem ) {
			ok = 1,
			m_ct->Expand( hItem, TVE_TOGGLE );
		}
	} else if( nChar == VK_TAB ) {
		CHFVExplorerApp	*pApp;
		pApp = (CHFVExplorerApp *)AfxGetApp();
        if(auto list=GetBrowserSession().List())list->SetFocus();
		ok = 1;
	}
	if(!ok)	CTreeView::OnChar(nChar, nRepCnt, nFlags);
}

BOOL HFVExplorerTreeView::is_cd_item( HTREEITEM hItem )
{
	DWORD_PTR dw;
	BOOL result = FALSE;
	int volinx;

	if(!hItem) return(FALSE);
	dw = m_ct->GetItemData( hItem );
	if(dw) {
		switch(get_lparam_type( dw )) {
   case LIST_MAC: return GetBrowserSession().MacBrowser().SelectedCdDrive()>=0;
			case LIST_HFS:
				volinx = get_lparam_volinx( dw );
				if(((CHFVExplorerDoc*)m_pDocument)->Mounts().ClassicAt(volinx).m_is_cd) result = TRUE;
				break;
			case LIST_FAT:
				char path[MAX_PATH];
				if(get_lparam_path( dw, path, MAX_PATH-1 )) {
					path[3] = 0;
					if(GetDriveType( path ) == DRIVE_CDROM) result = TRUE;
				}
				break;
		}
	}
	return(result);
}

void HFVExplorerTreeView::OnContextMenu(CWnd* pWnd, CPoint point) 
{
	// No tree context menu is currently provided.
}

BOOL HFVExplorerTreeView::OnCommand(WPARAM wParam, LPARAM lParam) 
{
	return CTreeView::OnCommand(wParam, lParam);
}

void HFVExplorerTreeView::delete_subdir_contents( HTREEITEM root )
{
	HTREEITEM h;
	int loopguard = 0;

	if(!root) return;
	while( m_ct->ItemHasChildren(root) ) {
		h = m_ct->GetChildItem( root );
		delete_subdir_contents( h );
		if(!m_ct->DeleteItem( h )) break;
	}
}

void HFVExplorerTreeView::on_volume_changed( int volinx )
{
	CString currdir;
	int mac;
	HTREEITEM root;
	CHFVExplorerApp	*pApp;

	pApp = (CHFVExplorerApp *)AfxGetApp();

	root = MountRoot({BrowserMountInventory::Kind::ClassicVolume,volinx});
	currdir = ((CHFVExplorerDoc*)m_pDocument)->get_directory( 0, &mac );
	GetBrowserSession().Tree()->enable_selchange(0);
	delete_subdir_contents( root );
	GetBrowserSession().Tree()->enable_selchange(1);
	if(currdir != "") {
		((CHFVExplorerDoc*)m_pDocument)->my_chdir( currdir, mac );
	}
}

void HFVExplorerTreeView::mode_changed( void )
{
	HTREEITEM hItem = GetTreeCtrl().GetSelectedItem();
	if(hItem) {
		GetTreeCtrl().SelectItem( 0 );
		GetTreeCtrl().EnsureVisible( hItem );
		GetTreeCtrl().SelectItem( hItem );
	}
}

/*
void HFVExplorerTreeView::delete_by_path( LPCSTR path )
{
	HTREEITEM htree;

	htree = tree_find_fat( MountRoot({BrowserMountInventory::Kind::ClassicVolume,pitem->volinx}), path );
	if(htree) {
		m_ct->DeleteItem(htree);
	}
}
*/
