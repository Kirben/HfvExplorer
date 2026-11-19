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
#include "DocumentLaunch.h"
#include "CommandStorage.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "BrowserTransfers.h"
#include "MainMacBrowser.h"
#include "RenameName.h"
#include "DeviceWait.h"
#include "CopyProgress.h"
#include "FileOperations.h"
#include "StorageJobDialog.h"
#include "HFVExplorer.h"
#include "HFVExplorerListView.h"
#include "HFVExplorerDoc.h"
#include "HFVPreviewView.h"
#include "MainFrm.h"
#include "special.h"
#include "own_ids.h"
#include "utils.h"
#include "shell.h"
#include <direct.h>
#include "hfs\libhfs\hfs.h"
#include "hfs\interface.h"
#include "AskDir.h"
#include "icon.h"
#include "floppy.h"
#include "FileTypeMapping.h"
#include "tmap.h"

extern "C" {
#include "hfs\copyout.h"
}

#ifdef _DEBUG
// #define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerListView

IMPLEMENT_DYNCREATE(CHFVExplorerListView, CListView)
namespace {
bool HiddenOperationProgress(CWnd* owner){
#ifdef HFV_TESTING
 return owner&&!owner->IsWindowVisible();
#else
 (void)owner;return false;
#endif
}
}


#define NCOLUMNS 6

LPARAM CHFVExplorerListView::StoreRow(BrowserDirectory::Row row)
{
 const auto key=LPARAM(m_browser_rows.size()+1);
 m_browser_rows.emplace(key,std::move(row));
 return key;
}

const BrowserDirectory::Row* CHFVExplorerListView::BoundRow(LPARAM key) const
{
 const auto found=m_browser_rows.find(key);
 return found==m_browser_rows.end()?nullptr:&found->second;
}

HICON CHFVExplorerListView::get_lparam_icon32(LPARAM key)
{
 const auto* row=BoundRow(key);
 return row?(row->icon32?row->icon32:row->icon16):nullptr;
}

long CHFVExplorerListView::get_lparam_id(LPARAM key)
{
 const auto* row=BoundRow(key);
 return row?long(row->entry.id):0;
}

int CHFVExplorerListView::get_lparam_isdir(LPARAM key)
{
 const auto* row=BoundRow(key);
 return row&&row->entry.directory;
}

int CHFVExplorerListView::get_inx_isdir(int index)
{
 const auto* row=BoundRow(m_cl->GetItemData(index));
 return row?int(row->entry.directory):-1;
}

int CHFVExplorerListView::get_lparam_type(LPARAM key)
{
 const auto* row=BoundRow(key);
 return row?(row->parentLink?LIST_SPEC:row->location.LegacyType()):LIST_UNKNOWN;
}

int CHFVExplorerListView::get_inx_type(int index)
{
 const auto* row=BoundRow(m_cl->GetItemData(index));
 return row?(row->parentLink?LIST_SPEC:row->location.LegacyType()):-1;
}

HTREEITEM CHFVExplorerListView::get_lparam_htree(LPARAM key)
{
 const auto* row=BoundRow(key);
 return row?row->treeItem:nullptr;
}

void CHFVExplorerListView::destroy_pointers()
{
 m_editing_item=-1;
 m_browser_rows.clear();
}

#define LISTICONCOUNT 12

CHFVExplorerListView::CHFVExplorerListView()
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	m_sorted_by_column = FIELD_NAME;
	m_sort_is_ascending = TRUE;


	m_move_cursor = pApp->LoadCursor( IDC_POINTER );
	m_copy_cursor = pApp->LoadCursor( IDC_POINTER_COPY );

	m_cl = &this->GetListCtrl();

	m_dragging = 0;

	m_item_count = 0;
    m_largeImages.Create(LISTICONCOUNT);
    m_smallImages.Create(LISTICONCOUNT);
	CreatePalette();

	m_largeImages.Add(pApp->LoadIcon(IDI_ICON1));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON2));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON3));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON4));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON5));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON6));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON7));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON8));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON9));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON10));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON11));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON12));
	m_largeImages.Add(pApp->LoadIcon(IDI_ICON1_YELLOW));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_PROJECT_2));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_PROJECT_1));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_PERSONAL));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_COOL));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_IN_PROGRESS));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_HOT));
	m_largeImages.Add(pApp->LoadIcon(IDI_LABEL_ESSENTIAL));

	m_smallImages.Add(pApp->LoadIcon(IDI_ICON1));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON2));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON3));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON4));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON5));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON6));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON7));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON8));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON9));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON10));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON11));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON12));
	m_smallImages.Add(pApp->LoadIcon(IDI_ICON1_YELLOW));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_PROJECT_2));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_PROJECT_1));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_PERSONAL));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_COOL));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_IN_PROGRESS));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_HOT));
	m_smallImages.Add(pApp->LoadIcon(IDI_LABEL_ESSENTIAL));

    m_largeImages.SetOverlays();
    m_smallImages.SetOverlays();
    m_builtinIconCount = m_largeImages.Count();

	m_pOldPalette = NULL;

 // The shared clipboard outlives pane initialization.
}

CHFVExplorerListView::~CHFVExplorerListView()
{
 CancelBrowserDrag();
 if(m_browserSession)m_browserSession->Detach(this);
	destroy_pointers();
}

void CHFVExplorerListView::ApplyDpi(UINT dpi)
{
    if (m_dpi == dpi) return;
    auto& app = *static_cast<CHFVExplorerApp*>(AfxGetApp());
    CancelBrowserDrag();
    const auto style = m_cl->GetStyle() & LVS_TYPEMASK;
    const int top = style == LVS_REPORT ? m_cl->GetTopIndex() : -1;
    std::vector<std::pair<int,CPoint>> positions;
    if (GetBrowserSession().finderLayout && (style == LVS_ICON || style == LVS_SMALLICON)) {
        for (int i = 0; i < m_cl->GetItemCount(); ++i) {
            CPoint point;
            if (!m_cl->GetItemPosition(i, &point)) continue;
            auto& axes = m_iconPositions[m_cl->GetItemData(i)];
            positions.emplace_back(i, CPoint(axes.first.Resize(point.x, m_dpi, dpi),
                axes.second.Resize(point.y, m_dpi, dpi)));
        }
    }
    m_largeImages.Apply(dpi, [&](HIMAGELIST images) {
        m_cl->SendMessage(LVM_SETIMAGELIST,LVSIL_NORMAL,LPARAM(images));
    });
    m_smallImages.Apply(dpi, [&](HIMAGELIST images) {
        m_cl->SendMessage(LVM_SETIMAGELIST,LVSIL_SMALL,LPARAM(images));
    });
    // Width messages in List mode address the icon column rather than
    // the actual Details columns. Read and write the column records.
    for (int i = 0; i < ARRAYSIZE(m_columns); ++i) {
        LVCOLUMN column{};
        column.mask = LVCF_WIDTH;
        if (!m_cl->GetColumn(i, &column)) continue;
        column.cx = m_columns[i].Resize(column.cx, m_dpi, dpi);
        m_cl->SetColumn(i, &column);
    }
    m_cl->SendMessage(WM_SETFONT, WPARAM(app.get_font(dpi)), TRUE);
    m_dpi = dpi;
    if (!positions.empty())
        for (const auto& position : positions)
            m_cl->SendMessage(LVM_SETITEMPOSITION32, position.first, LPARAM(&position.second));
    if (top > 0 && top != m_cl->GetTopIndex()) {
        CRect item;
        if (m_cl->GetItemRect(0, &item, LVIR_BOUNDS))
            m_cl->Scroll(CSize(0, (top-m_cl->GetTopIndex())*item.Height()));
    }
}

int CHFVExplorerListView::Initialize()
{
	LV_COLUMN		lvcolumn;
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

    m_cl->SendMessage(WM_SETFONT, WPARAM(pApp->get_font(m_dpi)), TRUE);

	// m_bkcolor = RGB( 255,255,192 );
	// m_cl->SetBkColor( m_bkcolor );

	list_clear();
    m_cl->ModifyStyle(0, LVS_SHAREIMAGELISTS);
	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Name";
	lvcolumn.iSubItem = FIELD_NAME;
	lvcolumn.cx = 170;
	m_cl->InsertColumn(FIELD_NAME, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Size";
	lvcolumn.iSubItem = FIELD_SIZE;
	lvcolumn.cx = 75;
	m_cl->InsertColumn(FIELD_SIZE, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Created";
	lvcolumn.iSubItem = FIELD_CREATED;
	lvcolumn.cx = 140; // 90;
	m_cl->InsertColumn(FIELD_CREATED, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Modified";
	lvcolumn.iSubItem = FIELD_MODIFIED;
	lvcolumn.cx = 140; // 90;
	m_cl->InsertColumn(FIELD_MODIFIED, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Type";
	lvcolumn.iSubItem = FIELD_TYPE;
	lvcolumn.cx = 50;
	m_cl->InsertColumn(FIELD_TYPE, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Creator";
	lvcolumn.iSubItem = FIELD_CREATOR;
	lvcolumn.cx = 65;
	m_cl->InsertColumn(FIELD_CREATOR, &lvcolumn);

	lvcolumn.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	lvcolumn.fmt = LVCFMT_LEFT;
	lvcolumn.pszText = "Label";
	lvcolumn.iSubItem = FIELD_LABEL;
	lvcolumn.cx = 85;
	m_cl->InsertColumn(FIELD_LABEL, &lvcolumn);

	// this is no limit, just allocates this much instead of small pieces
	m_cl->SetItemCount( 500 ); 
  
	// CreatePalette();
	{
		CWnd* pActiveWnd = CWnd::GetActiveWindow();
		if (pActiveWnd != NULL)
			pActiveWnd->SendMessage(WM_QUERYNEWPALETTE);
	}

	return(1);
}

void CHFVExplorerListView::set_style( long style )
{
	long lStyleOld;

	lStyleOld = GetWindowLong(m_cl->m_hWnd, GWL_STYLE);

	lStyleOld &= ~LVS_LIST;
	lStyleOld &= ~LVS_SMALLICON;
	lStyleOld &= ~LVS_OWNERDRAWFIXED;
	lStyleOld &= ~LVS_SORTDESCENDING;
	lStyleOld &= ~LVS_ALIGNLEFT;
	lStyleOld &= ~LVS_REPORT;
	// lStyleOld &= ~LVS_SHOWSELALWAYS;
	lStyleOld &= ~LVS_NOCOLUMNHEADER;

	lStyleOld &= ~LVS_AUTOARRANGE;
	lStyleOld &= ~(unsigned)LVS_NOSORTHEADER;

	lStyleOld |= (unsigned)LVS_NOSORTHEADER;
	// lStyleOld |= LVS_AUTOARRANGE;

	// lStyleOld |= LVS_SINGLESEL;

	lStyleOld |= LVS_EDITLABELS;
	lStyleOld |= LVS_SORTASCENDING;
	lStyleOld |= LVS_SHOWSELALWAYS;


	// TEST
	// lStyleOld |= LVS_OWNERDRAWFIXED;
	// lStyleOld |= LVS_NOLABELWRAP;


	lStyleOld |= style;

	SetWindowLong(m_cl->m_hWnd, GWL_STYLE, lStyleOld);

	switch( style ) {
		case LVS_LIST:
			break;
		case LVS_REPORT:
			do_report_sort();
			break;
		case LVS_ICON:
			break;
		case LVS_SMALLICON:
			break;
	}
}

int is_mac_path( CString &name )
{
	char *s;
	int i;

	s = name.GetBuffer(MAX_PATH);
	if( isalpha(s[0]) && s[1] == ':' && s[2] == '\\' )
		i = 0;
	else 
		i = 1;
	name.ReleaseBuffer();

	return(i);
}

int CHFVExplorerListView::initialize_volume(CString root_name,unsigned long id)
{
    InitializeFolderRows(id?std::optional<std::wstring>(VolumeCoord::Wide((LPCSTR)root_name)):std::nullopt,
        root_name.IsEmpty()||is_mac_path(root_name));
    return 1;
}

void CHFVExplorerListView::InitializeFolderRows(const std::optional<std::wstring>& parent,bool mac)
{
    list_clear();
    if(!parent||(mac&&GetBrowserSession().finderLayout))return;

    auto label=parent->empty()?std::wstring(L"Root folder"):L" ..up to "+*parent;
    BrowserDirectory::Row command;
    command.parentLink=true;
    command.entry.id=ID_GOTO_PARENT;
    command.entry.directory=true;
    LVITEMW item{};
    item.mask=LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM|LVIF_STATE;
    item.pszText=label.data();
    item.iImage=2;
    item.lParam=StoreRow(std::move(command));
    item.state=item.stateMask=LVIS_FOCUSED|LVIS_SELECTED;
    if(SendMessageW(m_cl->m_hWnd,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item))>=0){
        m_parentRowData=item.lParam;
        ++m_item_count;
    }else m_browser_rows.erase(item.lParam);
}

int CHFVExplorerListView::initialize_list()
{
	list_clear();
	return(1);
}

int CHFVExplorerListView::list_clear()
{
    CancelBrowserDrag();
    m_parentRowData=0;
    if(GetBrowserSession().Preview()&&IsWindow(GetBrowserSession().Preview()->GetSafeHwnd()))GetBrowserSession().Preview()->Clear();
	m_cl->DeleteAllItems();
    m_largeImages.Trim(m_builtinIconCount);
    m_smallImages.Trim(m_builtinIconCount);
    m_iconPositions.clear();
 destroy_pointers();m_directory_capabilities_known=false;
	m_item_count = 0;
	return(1);
}

// This origo is used to remove any Finder scrolling effect.
void CHFVExplorerListView::set_origo( int x, int y )
{
	m_mac_origo.x = x;
	m_mac_origo.y = y;
	// m_mac_origo.x = 0;
	// m_mac_origo.y = 0;
}

int CHFVExplorerListView::FillList()
{
    m_cl->SendMessage(LVM_SETIMAGELIST,LVSIL_NORMAL,LPARAM(m_largeImages.Handle()));
    m_cl->SendMessage(LVM_SETIMAGELIST,LVSIL_SMALL,LPARAM(m_smallImages.Handle()));
	return(1);
}

BEGIN_MESSAGE_MAP(CHFVExplorerListView, CListView)
	//{{AFX_MSG_MAP(CHFVExplorerListView)
	ON_WM_LBUTTONDBLCLK()
	ON_WM_CONTEXTMENU()
	ON_WM_SYSKEYDOWN()
	ON_WM_QUERYNEWPALETTE()
	ON_WM_PALETTECHANGED()
	ON_NOTIFY_REFLECT(LVN_BEGINDRAG, OnBegindrag)
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_NOTIFY_REFLECT(LVN_ITEMCHANGED, OnItemchanged)
	ON_WM_KEYDOWN()
 ON_WM_CAPTURECHANGED()
 ON_WM_CANCELMODE()
	ON_NOTIFY_REFLECT(LVN_ENDLABELEDIT, OnEndlabeledit)
	ON_NOTIFY_REFLECT(LVN_BEGINLABELEDIT, OnBeginlabeledit)
	ON_WM_KEYUP()
	ON_NOTIFY_REFLECT(LVN_COLUMNCLICK, OnColumnclick)
	ON_COMMAND(ID_EDIT_LABEL, OnEditLabel)
	ON_WM_MEASUREITEM()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerListView diagnostics

#ifdef _DEBUG
void CHFVExplorerListView::AssertValid() const
{
	CListView::AssertValid();
}

void CHFVExplorerListView::Dump(CDumpContext& dc) const
{
	CListView::Dump(dc);
}
#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerListView message handlers

int CHFVExplorerListView::get_path(int index, CString& path)
{
    path.Empty();
    const auto row=BrowserRowAt(index);
    if(!row||(!row->location.IsWindows()&&!row->location.IsClassic()))return 0;
    try{path=row->location.LegacyPath().c_str();}
    catch(const std::exception&){return 0;}
    return !path.IsEmpty();
}

BOOL CHFVExplorerListView::new_folder(void)
{
 auto& session=GetBrowserSession();
 auto& browser=session.MacBrowser();
 if(browser.NewFolder())return TRUE;
 if(!BrowserDirectoryWritable())return FALSE;

 try{
  const auto target=browser.TargetFolder();
  CAskDir dialog(session.Owner());
  dialog.m_new_dir_name="New Folder";
  for(int suffix=2;suffix<=50;++suffix){
   const auto proposed=VolumeCoord::Wide(dialog.m_new_dir_name.GetString());
   bool exists=false;
   for(int index=0;index<m_cl->GetItemCount();++index){
    const auto* row=BrowserRowAt(index);
    if(row&&CompareStringOrdinal(MacBrowser::Wide(row->entry.name).c_str(),-1,proposed.c_str(),-1,TRUE)==CSTR_EQUAL){
     exists=true;
     break;
    }
   }
   if(!exists)break;
   dialog.m_new_dir_name.Format("New Folder (%d)",suffix);
  }
  if(dialog.DoModal()!=IDOK)return FALSE;
  return browser.CreateFolder(target,BrowserDirectory::FromAnsi(dialog.m_new_dir_name.GetString()));
 }catch(const std::exception& error){
  session.Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return FALSE;
 }
}

int fat_rename( char *pOldName, char *pNewName )
{
	// Never let a global working-directory change select an unrelated entry.
	auto absolute=[](const char* p) {return p&&strlen(p)>=3&&((isalpha((unsigned char)p[0])&&p[1]==':'&&p[2]=='\\')||(p[0]=='\\'&&p[1]=='\\'));};
	if(!absolute(pOldName)||!absolute(pNewName))return 0;
	int result = 0;
	TRY
	{
		CFile::Rename( pOldName, pNewName );
		result = 1;
	}
	CATCH( CFileException, e )
	{
		result = 0;
	}
	END_CATCH
	return( result );
}

int CHFVExplorerListView::do_rename( 
	int item, 
	char *old_name,
	char *new_name,
	int check_lock
)
{
    if(!GetBrowserSession().ListingAvailable())return 0;
    const auto row=BrowserRowAt(item);
    if(!row||row->location.IsNone()||!old_name)return 0;
    auto visibleName=row->entry.name;
    if(row->entry.version)
        visibleName+=MacBrowser::U16(L" [version "+std::to_wstring(row->entry.version)+L"]");
    const auto inputName=BrowserDirectory::FromAnsi(old_name);
    if(inputName!=row->entry.name&&inputName!=visibleName)return 0;
    auto source=row->location;
    bool macRename=source.IsClassic();
    if(!HfvValidRenameLeaf(old_name,macRename)||!HfvValidRenameLeaf(new_name,macRename))return 0;
    if(BackgroundCopies::Enabled()){
        try{return GetBrowserSession().MacBrowser().RenameItem(item,BrowserDirectory::FromAnsi(new_name),check_lock!=0)?1:0;}
        catch(const std::exception& error){if(!HiddenOperationProgress(GetBrowserSession().Owner()))GetBrowserSession().Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return 0;}
    }
    const auto ds=LPARAM(m_cl->GetItemData(item));
    const auto oldPath=source.LegacyPath();

 FileOperations::RenameEntry request{source, BrowserDirectory::FromAnsi(new_name), {}, check_lock != 0};
 auto operation=FileOperations::Submit(std::move(request));auto outcome=ShowSubmittedStorageJob(operation.Job(),"Renaming",HiddenOperationProgress(GetBrowserSession().Owner()),true,true,GetBrowserSession().Owner());
 const bool result=outcome.code==0;if(outcome.code&&outcome.error!=ERROR_CANCELLED&&!HiddenOperationProgress(GetBrowserSession().Owner()))GetBrowserSession().Owner()->MessageBox(outcome.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
 if(result){std::string changed=oldPath;auto pos=changed.find_last_of(source.LegacyType()==LIST_HFS?':':'\\');if(pos!=std::string::npos)changed.resize(pos+1);changed+=new_name;
  auto found=m_browser_rows.find(LPARAM(ds));if(found!=m_browser_rows.end()){found->second.entry.name=BrowserDirectory::FromAnsi(new_name);
   auto& location=found->second.location;
   if(source.IsClassic()){
    location.SetClassic(source.ClassicImage(),changed);
    if(source.MediaGenerationKnown())location.BindMediaGeneration(source.MediaGeneration());
   }else location.SetWindows(VolumeCoord::Wide(changed));}
 }

 if(source.LegacyType()==LIST_HFS)update_floppy_view(const_cast<char*>(source.ClassicImage().c_str()));

	return(result);
}


bool CHFVExplorerListView::OpenWithDrop(const FileOperations::Location& source,
    const FileOperations::Location& application,bool mac)
{
 return DocumentLaunch::OpenWith(GetBrowserSession(),source,application,mac);
}

void CHFVExplorerListView::go_up( int control )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	HTREEITEM hItem = GetBrowserSession().Tree()->GetTreeCtrl().GetSelectedItem();

	if(hItem) {
		hItem = GetBrowserSession().Tree()->GetTreeCtrl().GetParentItem( hItem );
		if(hItem) {
			if(control) {
				GetBrowserSession().OpenFolderInNewWindow(hItem);
			} else {
				GetBrowserSession().Tree()->GetTreeCtrl().EnsureVisible( hItem );
				GetBrowserSession().Tree()->GetTreeCtrl().SelectItem( hItem );
			}
		}
	}
}

void CHFVExplorerListView::activate_list_item( int inx, int control, int make_shortcut ) 
{
 if(!control&&GetBrowserSession().MacBrowser().Activate(inx))return;
 if(control){
  const auto* folder=BrowserRowAt(inx);
  if(folder&&folder->entry.directory&&folder->treeItem&&!make_shortcut){GetBrowserSession().OpenFolderInNewWindow(folder->treeItem);return;}
  if(GetBrowserSession().MacBrowser().Activate(inx))return;
 }
    if(!GetBrowserSession().ListingAvailable())return;
    const auto row=BrowserRowAt(inx);
    const auto binding=row?0:m_cl->GetItemData(inx);
    if(!row&&(!binding||get_lparam_type(binding)!=LIST_SPEC))return;
    const int entry_type=row?row->location.LegacyType():LIST_SPEC;
    const auto id=row?row->entry.id:get_lparam_id(binding);
    int emulator=control<0?-control:0;

	if(entry_type == LIST_SPEC) {
		if(make_shortcut) {
			AfxMessageBox( "You must be kidding." );
			return;
		}
		if(id == ID_GOTO_PARENT) {
			go_up( control );
		}
		return;
	}

	if(row->entry.directory) {
		if(make_shortcut) {
			AfxMessageBox( "Cannot make shortcuts to folders." );
			return;
		}
		HTREEITEM hItem = row->treeItem;
		if(hItem) {
			if(control) {
				GetBrowserSession().OpenFolderInNewWindow(hItem);
			} else {
				GetBrowserSession().Tree()->GetTreeCtrl().EnsureVisible( hItem );
				GetBrowserSession().Tree()->GetTreeCtrl().SelectItem( hItem );
			}
		}
		return;
	}

	if(!VolumeCoord::Readable(row->location.IsWindows()?row->accessKey:row->location.expectedResource))return;
    DocumentLaunch::Request request;
    request.entry=row->entry;
    request.icons=row->ownedIcons;
    request.small=row->icon16;request.large=row->icon32;
    request.target=DocumentLaunch::FromMenuCommand(emulator);
    request.shortcut=make_shortcut!=FALSE;
    auto& session=GetBrowserSession();
    const auto* node=session.Tree()->BrowserNodeAt(session.Tree()->GetTreeCtrl().GetSelectedItem());
    if(row->location.IsClassic()&&node&&node->mount.kind==BrowserMountInventory::Kind::ClassicVolume)
        request.classicSlot=node->mount.index;
    try{request.selection=session.MacBrowser().CaptureItem(inx);}
    catch(const std::exception& error){session.Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return;}
    DocumentLaunch::Launch(session,std::move(request));
}

void CHFVExplorerListView::OnLButtonDblClk(UINT nFlags, CPoint point) 
{
	LV_HITTESTINFO ht;

	ht.pt = point;
	if( m_cl->HitTest( &ht ) >= 0 ) {
		if( ht.flags == LVHT_ONITEMICON || ht.flags == LVHT_ONITEMLABEL ||
				ht.flags == LVHT_ONITEMSTATEICON) {
			activate_list_item( ht.iItem, (nFlags & MK_CONTROL) != 0, FALSE );
			return;
		}
	}
	
	CListView::OnLButtonDblClk(nFlags, point);
}

#pragma optimize("g",off)
#pragma optimize("",on)

/*
int do_delete_fat_directory( char *path )
{
	HANDLE fh;
	WIN32_FIND_DATA FindFileData;
	int ok;
	char mask[_MAX_PATH];
	char new_source[_MAX_PATH];
	int result = 0;

	sprintf( mask, "%s\\*.*", path );
	fh = FindFirstFile( mask, &FindFileData );
	ok = fh != INVALID_HANDLE_VALUE;
	while(ok) {
		if(FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if(*FindFileData.cFileName != '.') {
				sprintf( new_source, "%s\\%s", path, FindFileData.cFileName );
				result = do_delete_fat_directory( new_source );
				if(result != 0) break;
			}
		} else {
			sprintf( new_source, "%s\\%s", path, FindFileData.cFileName );
			if(!DeleteFile(new_source)) {
				AfxMessageBox( "Could not delete directory " + CString(new_source) );
				result = 1;
				break;
			}
		}
		ok = FindNextFile( fh, &FindFileData );
	}
	if(fh != INVALID_HANDLE_VALUE) FindClose( fh );
	if(result == 0) {
		if(!RemoveDirectory(path)) {
			AfxMessageBox( "Could not delete directory " + CString(path) );
			result = 1;
		}
	}
	return(result);
}
*/

BOOL CHFVExplorerListView::map_selected_item( void )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CFileTypeMapping dlg;
	int inx, count;
	BOOL retval = FALSE;

	tmap_final();
	dlg.m_enabled_param = pApp->m_type_mapping_enabled;

	inx = -1; count = 0;
	while( (inx = m_cl->GetNextItem(inx,LVNI_SELECTED)) >= 0 ) {
        const auto row=BrowserRowAt(inx);
        if(!row||row->entry.directory)continue;
        if(row->location.IsClassic()){
            char type[5]{},creator[5]{};
            memcpy(type,row->entry.finder.data(),4);
            memcpy(creator,row->entry.finder.data()+4,4);
            dlg.m_input_types.Add(CString(type));
            dlg.m_input_creators.Add(CString(creator));
            dlg.m_input_dos.Add(CString("???"));
            ++count;
        }else if(row->location.IsWindows()){
            const auto& path=row->location.WindowsPath();
            const auto dot=path.find_last_of(L'.');
            const auto slash=path.find_last_of(L"\\\\/");
            if(dot==path.npos||dot+1==path.size()||(slash!=path.npos&&dot<slash))continue;
            CString extension(path.substr(dot+1).c_str());
            extension.MakeUpper();
            dlg.m_input_types.Add(CString("????"));
            dlg.m_input_creators.Add(CString("????"));
            dlg.m_input_dos.Add(extension);
            ++count;
        }
    }
	if(count > 0) {
		if (dlg.DoModal() == IDOK) {
			pApp->m_type_mapping_enabled = dlg.m_enabled_param;
			retval = TRUE;
		}
	} else {
		AfxMessageBox( "Please select at least one file to map. You cannot map folders or special buttons." );
	}
	tmap_init();
	return(retval);
}

#pragma optimize("g",off)
void CHFVExplorerListView::try_delete_selected_items( BOOL confirm )
{
 if(GetBrowserSession().MacBrowser().Delete(confirm!=FALSE))return;
 if(!BrowserSelectionAllows(false))return;
	int inx = -1, count = 0;
	int stype, isdir;
	CString nam, fullpath;
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	CDWordArray items;

	// Just get the count
	while( (inx = m_cl->GetNextItem(inx,LVNI_SELECTED)) >= 0 ) count++;

	if(count == 0) return;
	if(count == 1) {
		inx = m_cl->GetNextItem(-1,LVNI_SELECTED);
		stype = get_inx_type(inx);
		isdir = get_inx_isdir(inx);
		if(stype == LIST_SPEC) return;
		get_path( inx, fullpath );
		if(isdir) {
			nam = "Really delete folder \"" + fullpath + "\"?";
		} else {
			nam = "Really delete file \"" + fullpath + "\"?";
		}
	} else {
		nam.Format( "Really delete selected %d items?", count );
	}
	if(confirm) {
		if(GetBrowserSession().Owner()->MessageBox(nam,"HFVExplorer",MB_YESNO)==IDNO)
		{
			return;
		}
	}

 auto selection = GetBrowserSession().MacBrowser().SelectedItems(false);
 if (selection.items.empty())
  return;
 FileOperations::DeleteEntries request{std::move(selection.items)};
 if (BackgroundCopies::Enabled()) {
  BackgroundCopies::Start({std::move(request)}, GetBrowserSession().Owner(), true);
  return;
 }
 auto selected=request.selection;auto operation=FileOperations::Submit(std::move(request));
 auto result=ShowSubmittedStorageJob(operation.Job(),"Deleting files",HiddenOperationProgress(GetBrowserSession().Owner()),false,true,GetBrowserSession().Owner());
 if(result.code&&result.error!=ERROR_CANCELLED&&!HiddenOperationProgress(GetBrowserSession().Owner()))GetBrowserSession().Owner()->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
 bool host=false;for(auto& source:selected){if(source.LegacyType()==LIST_HFS)update_floppy_view(const_cast<char*>(source.ClassicImage().c_str()));else host=true;}
 if(host)GetBrowserSession().RefreshSelected();

}
#pragma optimize("",on)

void CHFVExplorerListView::toggle_label_colors()
{
    auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
    app->m_show_labels=!app->m_show_labels;
    for(int index=0;index<m_cl->GetItemCount();++index){
        const auto row=BrowserRowAt(index);
        const auto label=row&&app->m_show_labels?BrowserDirectory::Label(row->entry):0;
        m_cl->SetItemState(index,label?INDEXTOOVERLAYMASK(label):0,LVIS_OVERLAYMASK);
    }
}

void CHFVExplorerListView::set_label_color(int color)
{
    auto& session=GetBrowserSession();
    auto selection=session.MacBrowser().SelectedItems(false);
    std::vector<FileOperations::Request> requests;
    for(auto& item:selection.items)
        if(item.IsClassic())
            requests.emplace_back(FileOperations::EditClassicProperties{std::move(item),FileOperations::FinderLabel{color}});
    if(!requests.empty())BackgroundCopies::Start(std::move(requests),session.Owner(),true);
}

std::optional<bool> CHFVExplorerListView::BrowserItemCommandEnabled(UINT command) const
{
    auto& session = GetBrowserSession();
    auto& browser = session.MacBrowser();
    const int selected = m_cl->GetNextItem(-1, LVNI_SELECTED);
    const auto* row = BrowserRowAt(selected);
    const bool ready = session.ListingAvailable();
    switch (command) {
    case ID_OWN_FILE_OPEN:
    case ID_OWN_FOLDER_OPEN:
        return ready && ((row && !row->location.IsNone()) || IsParentRow(selected));
    case ID_OWN_FILE_PROPERTIES:
    case ID_OWN_FOLDER_PROPERTIES:
        return browser.CanInspectProperties(selected);
    case ID_OWN_NEW_FOLDER:
        return ready && (browser.Active() ? browser.CanCreateFolder() : BrowserDirectoryWritable());
    case ID_OWN_DELETE:
        return ready && (browser.Active() ? browser.SelectionWritable() : BrowserSelectionAllows(false));
    default:
        return std::nullopt;
    }
}

bool CHFVExplorerListView::RunBrowserItemCommand(UINT command)
{
    const auto enabled = BrowserItemCommandEnabled(command);
    if (!enabled) return false;
    if (!*enabled) return true;
    switch (command) {
    case ID_OWN_FILE_OPEN:
    case ID_OWN_FOLDER_OPEN:
        activate_list_item(m_cl->GetNextItem(-1, LVNI_SELECTED), FALSE, FALSE);
        break;
    case ID_OWN_FILE_PROPERTIES:
    case ID_OWN_FOLDER_PROPERTIES:
        on_properties(m_cl->GetNextItem(-1, LVNI_SELECTED));
        break;
    case ID_OWN_NEW_FOLDER:
        new_folder();
        break;
    case ID_OWN_DELETE:
        try_delete_selected_items(TRUE);
        break;
    }
    return true;
}

void CHFVExplorerListView::UpdateBrowserItemMenu(CMenu& menu) const
{
    for (UINT command : {ID_OWN_FILE_OPEN, ID_OWN_FOLDER_OPEN, ID_OWN_FILE_PROPERTIES,
                         ID_OWN_FOLDER_PROPERTIES, ID_OWN_NEW_FOLDER, ID_OWN_DELETE}) {
        if (menu.GetMenuState(command, MF_BYCOMMAND) != UINT(-1))
            menu.EnableMenuItem(command, MF_BYCOMMAND |
                (*BrowserItemCommandEnabled(command) ? MF_ENABLED : MF_GRAYED));
    }
}

void CHFVExplorerListView::OnContextMenu(CWnd* pWnd, CPoint point) 
{
 if(GetBrowserSession().MacBrowser().ContextMenu(point))return;
 if(!StorageSelectionAvailable()){
  if(point.x<0){CRect rect;m_cl->GetWindowRect(rect);point=rect.CenterPoint();}
  CMenu menu;menu.CreatePopupMenu();bool selected=m_cl->GetNextItem(-1,LVNI_SELECTED)>=0;
  menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_EDIT_COPY,"Copy");menu.AppendMenu(MF_STRING|(selected&&BrowserSelectionAllows(false)?MF_ENABLED:MF_GRAYED),ID_EDIT_CUT,"Cut");
  menu.AppendMenu(MF_STRING|(BrowserTransfers::Any()&&BrowserDirectoryWritable()?MF_ENABLED:MF_GRAYED),ID_EDIT_PASTE,"Paste");
  menu.AppendMenu(MF_STRING|(selected&&m_cl->GetSelectedCount()==1&&BrowserSelectionAllows(true)?MF_ENABLED:MF_GRAYED),ID_EDIT_LABEL,"Rename");
  menu.AppendMenu(MF_STRING|(BrowserDirectoryWritable()?MF_ENABLED:MF_GRAYED),ID_OWN_NEW_FOLDER,"New Folder...");
  menu.AppendMenu(MF_STRING|(selected?MF_ENABLED:MF_GRAYED),ID_OWN_FILE_PROPERTIES,"Properties...");
  menu.AppendMenu(MF_STRING|(selected&&BrowserSelectionAllows(false)?MF_ENABLED:MF_GRAYED),ID_OWN_DELETE,"Delete");
  UpdateBrowserItemMenu(menu);
  const auto command = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                                           GetBrowserSession().Owner());
  if (command) GetBrowserSession().Owner()->SendMessage(WM_COMMAND, command);
  return;
 }
	CMenu cm, cpop, cpop2, cpop3;
	int inx, hit = 0;
	int pop_created = 0;
	int pop_created2 = 0;
	int pop_created3 = 0;
	long id; DWORD_PTR dat;
	// CString str;
	char *label_string;

	inx = m_cl->GetNextItem( -1, LVNI_ALL | LVNI_SELECTED );
	if(inx >= 0) hit = 1;

	if(((CHFVExplorerApp *)AfxGetApp())->m_show_labels)
		label_string = "Hide labels";
	else
		label_string = "Show labels";

	if(cm.CreatePopupMenu()) {
		if(hit) {
			dat = m_cl->GetItemData( inx );
			id = get_lparam_id(dat);
			if(get_lparam_isdir(dat)) {
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FOLDER_OPEN, "&Open Folder\tEnter" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_DELETE, "&Delete\tDel" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_MAP_THIS, "&Map selected files...\tF4" );
				cm.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_NEW_FOLDER, "&New Folder..." );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SAVE_ICON, "Save &icon..." );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_TOGGLE_LABEL_COLORS, label_string );
				if(cpop3.CreatePopupMenu()) {
					pop_created3 = 1;
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_NONE, get_label_text(FINDER_COLOR_NONE) );
					cpop3.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_ESSENTIAL, get_label_text(FINDER_COLOR_ESSENTIAL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_HOT, get_label_text(FINDER_COLOR_HOT) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_IN_PROGRESS, get_label_text(FINDER_COLOR_IN_PROGRESS) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_COOL, get_label_text(FINDER_COLOR_COOL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PERSONAL, get_label_text(FINDER_COLOR_PERSONAL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PROJECT_1, get_label_text(FINDER_COLOR_PROJECT_1) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PROJECT_2, get_label_text(FINDER_COLOR_PROJECT_2) );
					cm.AppendMenu( MF_POPUP | MF_STRING | MF_ENABLED, (UINT_PTR)cpop3.m_hMenu, "Label..." );
				}
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_PROGRAM_OPTIONS, "&Options..." );
				cm.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FOLDER_PROPERTIES, "&Properties..." );
			} else {
				CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
				if(pApp->m_default_emulator == 0) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (E/DOS)\tEnter" );
				} else if(pApp->m_default_emulator == 1) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (E/Win32)\tEnter" );
				} else if(pApp->m_default_emulator == 2) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (vMac)\tEnter" );
				} else if(pApp->m_default_emulator == 3) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (Shapeshifter)\tEnter" );
				} else if(pApp->m_default_emulator == 4) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (Fusion DOS)\tEnter" );
				} else if(pApp->m_default_emulator == 5) {
					cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_OPEN, "&Open (Associated Windows program)\tEnter" );
				}
				
				/*
				if(cpop.CreatePopupMenu()) {
					pop_created = 1;
					// mac
					cpop.AppendMenu( MF_STRING | MF_DISABLED | MF_GRAYED, ID_OWN_WITH_MAC_FIRST, "TextPad" );
					cpop.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
					// dos
					cpop.AppendMenu( MF_STRING | MF_DISABLED | MF_GRAYED, ID_OWN_WITH_PC_FIRST, "PC: WordPad" );
					cpop.AppendMenu( MF_STRING | MF_DISABLED | MF_GRAYED, ID_OWN_WITH_PC_FIRST+1, "PC: NotePad" );
					cpop.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
					cpop.AppendMenu( MF_STRING | MF_DISABLED | MF_GRAYED, ID_OWN_WITH_LIST, "Edit list..." );
				}
				*/
				if(cpop.CreatePopupMenu()) {
					pop_created = 1;
					// mac
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+0, "Executor &DOS" );
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+1, "Executor &Win32" );
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+2, "&vMac" );
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+3, "&Shapeshifter" );
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+4, "&Fusion DOS" );
					cpop.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_WITH_MAC_FIRST+5, "&Associated Windows program" );
				}
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_DELETE, "&Delete\tDel" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_MAP_THIS, "&Map selected files..\tF4" );
				
				if(pop_created) cm.AppendMenu( MF_POPUP | MF_STRING | MF_ENABLED, (UINT_PTR)cpop.m_hMenu, "Open &with..." );
				if(cpop2.CreatePopupMenu()) {
					pop_created2 = 1;
					cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_CREATE_E_DOS, "Executor &DOS" );
					cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_CREATE_E_WIN32, "Executor &Win32" );
					cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_CREATE_VMAC, "&vMac" );
					cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_CREATE_SS, "&Shapeshifter" );
					cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_CREATE_FUSION, "&Fusion DOS" );
					// cpop2.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_WINDOWS, "&Associated Windows program" );
					cm.AppendMenu( MF_POPUP | MF_STRING | MF_ENABLED, (UINT_PTR)cpop2.m_hMenu, "Create &Shortcut for..." );
				}
				cm.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_NEW_FOLDER, "&New Folder..." );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SAVE_ICON, "Save &icon..." );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_TOGGLE_LABEL_COLORS, label_string );
				if(cpop3.CreatePopupMenu()) {
					pop_created3 = 1;
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_NONE, get_label_text(FINDER_COLOR_NONE) );
					cpop3.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_ESSENTIAL, get_label_text(FINDER_COLOR_ESSENTIAL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_HOT, get_label_text(FINDER_COLOR_HOT) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_IN_PROGRESS, get_label_text(FINDER_COLOR_IN_PROGRESS) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_COOL, get_label_text(FINDER_COLOR_COOL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PERSONAL, get_label_text(FINDER_COLOR_PERSONAL) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PROJECT_1, get_label_text(FINDER_COLOR_PROJECT_1) );
					cpop3.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_SHORTCUT_COLOR_PROJECT_2, get_label_text(FINDER_COLOR_PROJECT_2) );
					cm.AppendMenu( MF_POPUP | MF_STRING | MF_ENABLED, (UINT_PTR)cpop3.m_hMenu, "Label..." );
				}
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_PROGRAM_OPTIONS, "&Options..." );
				cm.AppendMenu( MF_SEPARATOR | MF_DISABLED | MF_GRAYED, 0, "" );
				cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_FILE_PROPERTIES, "&Properties..." );
			}
		} else {
			cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_NEW_FOLDER, "&New Folder..." );
			cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_TOGGLE_LABEL_COLORS, label_string );
			cm.AppendMenu( MF_STRING | MF_ENABLED, ID_OWN_PROGRAM_OPTIONS, "&Options..." );
		}
		UpdateBrowserItemMenu(cm);
		if(cm.TrackPopupMenu( TPM_LEFTALIGN, point.x, point.y, this )) {
		}
		if(pop_created) cpop.DestroyMenu();
		if(pop_created2) cpop2.DestroyMenu();
		if(pop_created3) cpop3.DestroyMenu();
		cm.DestroyMenu();
	}
}

void CHFVExplorerListView::OnSysKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags ) 
{
	CPoint point;
	int inx;
	RECT rect;

	inx = m_cl->GetNextItem( -1, LVNI_ALL | LVNI_SELECTED );
	if( inx >= 0 && nChar == VK_RETURN ) {
		if(m_cl->GetItemRect( inx, &rect, LVIR_BOUNDS )) {
			point.x = (rect.left + rect.right) / 2;
			point.y = (rect.top + rect.bottom) / 2;
			ClientToScreen( &point );
			OnContextMenu( this, point );
		}
	} else {
		CListView::OnSysKeyDown(nChar, nRepCnt, nFlags);
	}
}

void CHFVExplorerListView::on_properties(int index)
{
    GetBrowserSession().MacBrowser().Properties(index);
}

#define MAXCMDS 40


void CHFVExplorerListView::save_icon_command( int inx )
{
	CString path;

	CFileDialog dlg( FALSE, _T("ICO"), path,
				OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
				_T("Icon Files|*.ico|All Files|*.*||") );
	if(dlg.DoModal() == IDOK) {
		path = dlg.GetPathName();
		const auto* row=BrowserRowAt(inx);
        if(!row||!DocumentLaunch::SaveFileIcon(row->icon16,row->icon32,path,false)) {
			CString cs = "Failed to save the icon to file \"" + CString(path) + ".\"";
			AfxMessageBox( cs );
		}
	}
}

BOOL CHFVExplorerListView::OnCommand(WPARAM wParam, LPARAM lParam) 
{
 if (RunBrowserItemCommand(LOWORD(wParam))) return TRUE;
 if(GetBrowserSession().MacBrowser().Command(LOWORD(wParam)))return TRUE;
	int handled = 1;
	UINT cmd = LOWORD(wParam);
	int inx;

	switch( cmd ) {
		case ID_OWN_SHORTCUT_COLOR_NONE:
			set_label_color( FINDER_COLOR_NONE );
			break;
		case ID_OWN_SHORTCUT_COLOR_ESSENTIAL:
			set_label_color( FINDER_COLOR_ESSENTIAL );
			break;
		case ID_OWN_SHORTCUT_COLOR_HOT:
			set_label_color( FINDER_COLOR_HOT );
			break;
		case ID_OWN_SHORTCUT_COLOR_IN_PROGRESS:
			set_label_color( FINDER_COLOR_IN_PROGRESS );
			break;
		case ID_OWN_SHORTCUT_COLOR_COOL:
			set_label_color( FINDER_COLOR_COOL );
			break;
		case ID_OWN_SHORTCUT_COLOR_PERSONAL:
			set_label_color( FINDER_COLOR_PERSONAL );
			break;
		case ID_OWN_SHORTCUT_COLOR_PROJECT_1:
			set_label_color( FINDER_COLOR_PROJECT_1 );
			break;
		case ID_OWN_SHORTCUT_COLOR_PROJECT_2:
			set_label_color( FINDER_COLOR_PROJECT_2 );
			break;
		case ID_OWN_TOGGLE_LABEL_COLORS:
			toggle_label_colors();
			break;
		case ID_OWN_MAP_THIS:
			inx = m_cl->GetNextItem( -1, LVNI_SELECTED );
			if(inx >= 0) {
				map_selected_item();
			}
			break;
		case ID_OWN_SAVE_ICON:
			inx = m_cl->GetNextItem( -1, LVNI_SELECTED );
			if(inx >= 0) {
				save_icon_command(inx);
			}
			break;
		case ID_OWN_PROGRAM_OPTIONS:
			{
			CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
			static_cast<CMainFrame*>(GetBrowserSession().Owner())->OnProgramProperties();
		  // ((CHFVExplorerDoc*)m_pDocument)->open_sub_directory( id, name );
			}
			break;
		case ID_OWN_WITH_LIST:
			break;
		case ID_OWN_SHORTCUT_CREATE_E_DOS:
		case ID_OWN_SHORTCUT_CREATE_E_WIN32:
		case ID_OWN_SHORTCUT_CREATE_VMAC:
		case ID_OWN_SHORTCUT_CREATE_SS:
		case ID_OWN_SHORTCUT_CREATE_FUSION:
		case ID_OWN_SHORTCUT_WINDOWS:
			inx = m_cl->GetNextItem( -1, LVNI_ALL | LVNI_FOCUSED );
			if(inx >= 0) {
				activate_list_item( inx, -(int)cmd, TRUE );
			}
			break;
		default:
			// if( cmd >= ID_OWN_WITH_MAC_FIRST && cmd <= ID_OWN_WITH_MAC_FIRST+MAXCMDS) {
			if( cmd >= ID_OWN_WITH_MAC_FIRST && cmd <= ID_OWN_WITH_MAC_FIRST+5) {
				inx = m_cl->GetNextItem( -1, LVNI_ALL | LVNI_FOCUSED );
				if(inx >= 0) {
					switch(cmd) {
						case ID_OWN_WITH_MAC_FIRST:
							activate_list_item( inx, -ID_OWN_SHORTCUT_CREATE_E_DOS, FALSE );
							break;
						case ID_OWN_WITH_MAC_FIRST+1:
							activate_list_item( inx, -ID_OWN_SHORTCUT_CREATE_E_WIN32, FALSE );
							break;
						case ID_OWN_WITH_MAC_FIRST+2:
							activate_list_item( inx, -ID_OWN_SHORTCUT_CREATE_VMAC, FALSE );
							break;
						case ID_OWN_WITH_MAC_FIRST+3:
							activate_list_item( inx, -ID_OWN_SHORTCUT_CREATE_SS, FALSE );
							break;
						case ID_OWN_WITH_MAC_FIRST+4:
							activate_list_item( inx, -ID_OWN_SHORTCUT_CREATE_FUSION, FALSE );
							break;
						case ID_OWN_WITH_MAC_FIRST+5:
							activate_list_item( inx, -ID_OWN_SHORTCUT_WINDOWS, FALSE );
							break;
					}
				}
			} else if( cmd >= ID_OWN_WITH_PC_FIRST && cmd <= ID_OWN_WITH_PC_FIRST+MAXCMDS) {
			} else {
				handled = 0;
			}
	}
	if(handled) 
		return(1);
	else
		return CListView::OnCommand(wParam, lParam);
}

RGBQUAD rgbStd256[] =
{
	{   0,  0,  0, 0 },	{   0,  0,128, 0 },	{   0,128,  0, 0 },	{   0,128,128, 0 },
	{ 128,  0,  0, 0 },	{ 128,  0,128, 0 },	{ 128,128,  0, 0 },	{ 192,192,192, 0 },
	{ 192,220,192, 0 },	{ 240,202,166, 0 },	{ 238,238,238, 0 },	{ 221,221,221, 0 },
	{ 204,204,204, 0 },	{ 187,187,187, 0 },	{ 170,170,170, 0 },	{ 153,153,153, 0 },
	{ 136,136,136, 0 },	{ 119,119,119, 0 },	{ 102,102,102, 0 },	{  85, 85, 85, 0 },
	{  68, 68, 68, 0 },	{  51, 51, 51, 0 },	{  34, 34, 34, 0 },	{  17, 17, 17, 0 },
	{ 204,255,255, 0 },	{ 153,255,255, 0 },	{ 102,255,255, 0 },	{  51,255,255, 0 },
	{ 255,204,255, 0 },	{ 204,204,255, 0 },	{ 153,204,255, 0 },	{ 102,204,255, 0 },
	{  51,204,255, 0 },	{   0,204,255, 0 },	{ 255,153,255, 0 },	{ 204,153,255, 0 },
	{ 153,153,255, 0 },	{ 102,153,255, 0 },	{  51,153,255, 0 },	{   0,153,255, 0 },
	{ 255,102,255, 0 },	{ 204,102,255, 0 },	{ 153,102,255, 0 },	{ 102,102,255, 0 },
	{  51,102,255, 0 },	{   0,102,255, 0 },	{ 255, 51,255, 0 },	{ 204, 51,255, 0 },
	{ 153, 51,255, 0 },	{ 102, 51,255, 0 },	{  51, 51,255, 0 },	{   0, 51,255, 0 },
	{ 204,  0,255, 0 },	{ 153,  0,255, 0 },	{ 102,  0,255, 0 },	{  51,  0,255, 0 },
	{ 255,255,204, 0 },	{ 204,255,204, 0 },	{ 153,255,204, 0 },	{ 102,255,204, 0 },
	{  51,255,204, 0 },	{   0,255,204, 0 },	{ 255,204,204, 0 },	{ 153,204,204, 0 },
	{ 102,204,204, 0 },	{  51,204,204, 0 },	{   0,204,204, 0 },	{ 255,153,204, 0 },
	{ 204,153,204, 0 },	{ 153,153,204, 0 },	{ 102,153,204, 0 },	{  51,153,204, 0 },
	{   0,153,204, 0 },	{ 255,102,204, 0 },	{ 204,102,204, 0 },	{ 153,102,204, 0 },
	{ 102,102,204, 0 },	{  51,102,204, 0 },	{   0,102,204, 0 },	{ 255, 51,204, 0 },
	{ 204, 51,204, 0 },	{ 153, 51,204, 0 },	{ 102, 51,204, 0 },	{  51, 51,204, 0 },
	{   0, 51,204, 0 },	{ 255,  0,204, 0 },	{ 204,  0,204, 0 },	{ 153,  0,204, 0 },
	{ 102,  0,204, 0 },	{  51,  0,204, 0 },	{ 255,255,153, 0 },	{ 204,255,153, 0 },
	{ 153,255,153, 0 },	{ 102,255,153, 0 },	{  51,255,153, 0 },	{   0,255,153, 0 },
	{ 255,204,153, 0 },	{ 204,204,153, 0 },	{ 153,204,153, 0 },	{ 102,204,153, 0 },
	{  51,204,153, 0 },	{   0,204,153, 0 },	{ 255,153,153, 0 },	{ 204,153,153, 0 },
	{ 102,153,153, 0 },	{  51,153,153, 0 },	{   0,153,153, 0 },	{ 255,102,153, 0 },
	{ 204,102,153, 0 },	{ 153,102,153, 0 },	{ 102,102,153, 0 },	{  51,102,153, 0 },
	{   0,102,153, 0 },	{ 255, 51,153, 0 },	{ 204, 51,153, 0 },	{ 153, 51,153, 0 },
	{ 102, 51,153, 0 },	{  51, 51,153, 0 },	{   0, 51,153, 0 },	{ 255,  0,153, 0 },
	{ 204,  0,153, 0 },	{ 153,  0,153, 0 },	{ 102,  0,153, 0 },	{  51,  0,153, 0 },
	{ 255,255,102, 0 },	{ 204,255,102, 0 },	{ 153,255,102, 0 },	{ 102,255,102, 0 },
	{  51,255,102, 0 },	{   0,255,102, 0 },	{ 255,204,102, 0 },	{ 204,204,102, 0 },
	{ 153,204,102, 0 },	{ 102,204,102, 0 },	{  51,204,102, 0 },	{   0,204,102, 0 },
	{ 255,153,102, 0 },	{ 204,153,102, 0 },	{ 153,153,102, 0 },	{ 102,153,102, 0 },
	{  51,153,102, 0 },	{   0,153,102, 0 },	{ 255,102,102, 0 },	{ 204,102,102, 0 },
	{ 153,102,102, 0 },	{  51,102,102, 0 },	{   0,102,102, 0 },	{ 255, 51,102, 0 },
	{ 204, 51,102, 0 },	{ 153, 51,102, 0 },	{ 102, 51,102, 0 },	{  51, 51,102, 0 },
	{   0, 51,102, 0 },	{ 255,  0,102, 0 },	{ 204,  0,102, 0 },	{ 153,  0,102, 0 },
	{ 102,  0,102, 0 },	{  51,  0,102, 0 },	{ 255,255, 51, 0 },	{ 204,255, 51, 0 },
	{ 153,255, 51, 0 },	{ 102,255, 51, 0 },	{  51,255, 51, 0 },	{   0,255, 51, 0 },
	{ 255,204, 51, 0 },	{ 204,204, 51, 0 },	{ 153,204, 51, 0 },	{ 102,204, 51, 0 },
	{  51,204, 51, 0 },	{   0,204, 51, 0 },	{ 255,153, 51, 0 },	{ 204,153, 51, 0 },
	{ 153,153, 51, 0 },	{ 102,153, 51, 0 },	{  51,153, 51, 0 },	{   0,153, 51, 0 },
	{ 255,102, 51, 0 },	{ 204,102, 51, 0 },	{ 153,102, 51, 0 },	{ 102,102, 51, 0 },
	{  51,102, 51, 0 },	{   0,102, 51, 0 },	{ 255, 51, 51, 0 },	{ 204, 51, 51, 0 },
	{ 153, 51, 51, 0 },	{ 102, 51, 51, 0 },	{   0, 51, 51, 0 },	{ 255,  0, 51, 0 },
	{ 204,  0, 51, 0 },	{ 153,  0, 51, 0 },	{ 102,  0, 51, 0 },	{  51,  0, 51, 0 },
	{ 204,255,  0, 0 },	{ 153,255,  0, 0 },	{ 102,255,  0, 0 },	{  51,255,  0, 0 },
	{ 255,204,  0, 0 },	{ 204,204,  0, 0 },	{ 153,204,  0, 0 },	{ 102,204,  0, 0 },
	{  51,204,  0, 0 },	{ 255,153,  0, 0 },	{ 204,153,  0, 0 },	{ 153,153,  0, 0 },
	{ 102,153,  0, 0 },	{   0,  0,238, 0 },	{   0,  0,221, 0 },	{   0,  0,204, 0 },
	{   0,  0,187, 0 },	{   0,  0,170, 0 },	{   0,  0,153, 0 },	{   0,  0,136, 0 },
	{   0,  0,119, 0 },	{   0,  0,102, 0 },	{   0,  0, 85, 0 },	{   0,  0, 68, 0 },
	{   0,  0, 51, 0 },	{   0,  0, 34, 0 },	{   0,  0, 17, 0 },	{   0,238,  0, 0 },
	{   0,221,  0, 0 },	{   0,204,  0, 0 },	{   0,187,  0, 0 },	{   0,170,  0, 0 },
	{   0,153,  0, 0 },	{   0,136,  0, 0 },	{   0,119,  0, 0 },	{   0,102,  0, 0 },
	{   0, 85,  0, 0 },	{   0, 68,  0, 0 },	{   0, 51,  0, 0 },	{   0, 34,  0, 0 },
	{   0, 17,  0, 0 },	{ 238,  0,  0, 0 },	{ 221,  0,  0, 0 },	{ 204,  0,  0, 0 },
	{ 187,  0,  0, 0 },	{ 170,  0,  0, 0 },	{ 153,  0,  0, 0 },	{ 136,  0,  0, 0 },
	{ 119,  0,  0, 0 },	{ 102,  0,  0, 0 },	{  85,  0,  0, 0 },	{  68,  0,  0, 0 },
	{  51,  0,  0, 0 },	{  34,  0,  0, 0 },	{ 240,251,255, 0 },	{ 164,160,160, 0 },
	{ 128,128,128, 0 },	{   0,  0,255, 0 },	{   0,255,  0, 0 },	{   0,255,255, 0 },
	{ 255,  0,  0, 0 },	{ 255,  0,255, 0 },	{ 255,255,  0, 0 },	{ 255,255,255, 0 }
};					   

RGBQUAD mac256[] =
{
	{ 255,255,255, 0 },	{ 253,203,153, 0 },	{ 253,203,153, 0 },	{ 253,203, 53, 0 },
	{ 253,203, 53, 0 },	{ 253,203,  0, 0 },	{ 203,150,160, 0 },	{ 203,203,203, 0 },
	{ 210,153,153, 0 },	{ 225,152, 55, 0 },	{ 230,152, 55, 0 },	{ 208,150,  0, 0 },
	{ 204,152,165, 0 },	{ 217,152,152, 0 },	{ 208,152,153, 0 },	{ 223,152, 53, 0 },
	{ 223,152, 55, 0 },	{ 208,151,  0, 0 },	{ 203, 52,106, 0 },	{ 202, 51,107, 0 },
	{ 204, 51,108, 0 },	{ 204, 51, 53, 0 },	{ 203, 50, 51, 0 },	{ 205, 95,  3, 0 },
	{ 203, 50,105, 0 },	{ 204, 50,105, 0 },	{ 203, 50,105, 0 },	{ 203, 51, 51, 0 },
	{ 204, 51, 52, 0 },	{ 203, 96,  0, 0 },	{ 175,  0,103, 0 },	{ 200,  0,102, 0 },
	{ 200,  0,102, 0 },	{ 200,  0, 51, 0 },	{ 201,  0, 52, 0 },	{ 203, 40, 18, 0 },
	{ 205,205,205, 0 },	{ 203,203,152, 0 },	{ 203,203,152, 0 },	{ 203,203, 51, 0 },
	{ 203,203, 50, 0 },	{ 203,203,  0, 0 },	{ 199,152,176, 0 },	{ 203,203,203, 0 },
	{ 185,185,185, 0 },	{ 205,151, 53, 0 },	{ 205,152, 54, 0 },	{ 203,153,  0, 0 },
	{ 200,152,175, 0 },	{ 193,193,193, 0 },	{ 203,153,153, 0 },	{ 203,153, 53, 0 },
	{ 203,153, 55, 0 },	{ 203,153,  0, 0 },	{ 153, 49,103, 0 },	{ 203, 51,112, 0 },
	{ 203, 51,111, 0 },	{ 203, 65, 63, 0 },	{ 203, 63, 63, 0 },	{ 206, 87,  5, 0 },
	{ 153, 48,104, 0 },	{ 203, 51,113, 0 },	{ 203, 51,106, 0 },	{ 203, 52, 54, 0 },
	{ 204, 52, 57, 0 },	{ 203, 86,  5, 0 },	{ 153,  0,102, 0 },	{ 167,  0,101, 0 },
	{ 170,  0,101, 0 },	{ 195,  0, 53, 0 },	{ 196,  0, 51, 0 },	{ 202,  0, 18, 0 },
	{ 153,203,205, 0 },	{ 148,182,103, 0 },	{ 147,181,103, 0 },	{ 150,174, 43, 0 },
	{ 151,173, 46, 0 },	{ 153,201,  0, 0 },	{ 106,152,204, 0 },	{ 190,190,190, 0 },
	{ 150,150,150, 0 },	{ 147,151, 57, 0 },	{ 147,152, 52, 0 },	{ 152,153,  0, 0 },
	{ 106,152,203, 0 },	{ 149,152,155, 0 },	{ 152,152,152, 0 },	{ 145,151, 65, 0 },
	{ 145,152, 64, 0 },	{ 150,153,  2, 0 },	{ 103, 50,121, 0 },	{ 118, 51,118, 0 },
	{ 124, 51,124, 0 },	{ 140, 63, 63, 0 },	{ 140, 51, 54, 0 },	{ 153, 71,  0, 0 },
	{ 102, 50,104, 0 },	{ 117, 52,118, 0 },	{ 120, 50,119, 0 },	{ 143, 51, 53, 0 },
	{ 141, 51, 55, 0 },	{ 152, 70,  2, 0 },	{ 103,  0, 99, 0 },	{ 103,  0,101, 0 },
	{ 103,  0,100, 0 },	{ 150,  0, 69, 0 },	{ 150,  0, 68, 0 },	{ 152,  0,  0, 0 },
	{ 103,169,170, 0 },	{ 102,152,101, 0 },	{ 103,152,101, 0 },	{ 102,152,  0, 0 },
	{ 101,152,  2, 0 },	{ 146,164,  0, 0 },	{ 103,152,205, 0 },	{ 100,152,154, 0 },
	{ 101,153,153, 0 },	{ 102,152, 56, 0 },	{ 102,151, 55, 0 },	{ 101,152,  0, 0 },
	{ 103,152,204, 0 },	{ 101,152,154, 0 },	{ 101,152,154, 0 },	{ 102,151, 55, 0 },
	{ 102,151, 55, 0 },	{ 101,151,  0, 0 },	{ 101, 50,105, 0 },	{ 101, 52,140, 0 },
	{ 101, 51,142, 0 },	{ 102,102,102, 0 },	{ 100, 52, 54, 0 },	{ 100, 69,  0, 0 },
	{ 101, 49,109, 0 },	{ 102, 50,143, 0 },	{ 101, 51,145, 0 },	{ 101, 52, 54, 0 },
	{  52, 52, 52, 0 },	{ 101, 72,  0, 0 },	{  90,  0, 93, 0 },	{  92,  0, 93, 0 },
	{  91,  0, 90, 0 },	{ 101,  0, 57, 0 },	{ 101,  0, 58, 0 },	{ 101,  0,  0, 0 },
	{ 101,153,156, 0 },	{  53,152, 99, 0 },	{  52,152, 97, 0 },	{ 100,151,  4, 0 },
	{  99,151,  4, 0 },	{ 102,151,  0, 0 },	{  51,151,204, 0 },	{  51,152,153, 0 },
	{  52,152,153, 0 },	{  51,152, 52, 0 },	{  51,151, 52, 0 },	{  69,152, 18, 0 },
	{  51,151,205, 0 },	{  52,152,152, 0 },	{  51,153,152, 0 },	{  51,152, 51, 0 },
	{  52,151, 52, 0 },	{  64,152, 15, 0 },	{  51, 50,105, 0 },	{  52, 50,143, 0 },
	{  51, 51,148, 0 },	{  51, 50, 54, 0 },	{  51, 49, 51, 0 },	{  51, 62,  0, 0 },
	{  52, 50,112, 0 },	{  51, 51,146, 0 },	{  52, 50,146, 0 },	{  51, 49, 51, 0 },
	{  52, 50, 52, 0 },	{  50, 70,  0, 0 },	{  50,  0, 75, 0 },	{  50,  0, 83, 0 },
	{  50,  0, 80, 0 },	{  50,  0, 65, 0 },	{  50,  0, 65, 0 },	{  50,  0,  0, 0 },
	{  65,152,152, 0 },	{  50,151, 54, 0 },	{  50,153, 56, 0 },	{  50,152, 45, 0 },
	{  50,152, 47, 0 },	{ 103,151,  0, 0 },	{   0,151,204, 0 },	{  16,150,146, 0 },
	{  15,151,146, 0 },	{   0,110, 51, 0 },	{   0,100, 51, 0 },	{  50,152, 50, 0 },
	{   0,151,204, 0 },	{   0,150,146, 0 },	{   0,150,150, 0 },	{   0,101, 51, 0 },
	{   0,102, 51, 0 },	{  51,152, 50, 0 },	{  42, 49,104, 0 },	{   0, 51,148, 0 },
	{   0, 51,146, 0 },	{   0, 55, 55, 0 },	{   0, 55, 55, 0 },	{   0,100, 50, 0 },
	{  46, 50,103, 0 },	{   0, 51,147, 0 },	{   0, 49,146, 0 },	{   0, 52, 55, 0 },
	{  50, 50, 50, 0 },	{   0,100, 50, 0 },	{  50,  0, 70, 0 },	{   0,  0, 70, 0 },
	{   0,  0, 70, 0 },	{   0,  0, 65, 0 },	{   0,  0, 65, 0 },	{ 203, 40, 20, 0 },
	{ 200,  0, 15, 0 },	{ 193,  0, 23, 0 },	{ 195,  0, 27, 0 },	{ 153,  0,  3, 0 },
	{ 102,  0,  0, 0 },	{  80,  0,  0, 0 },	{  55,  0,  0, 0 },	{   0,  0,  0, 0 },
	{   0,  0,  0, 0 },	{ 101,152,  0, 0 },	{ 101,151,  0, 0 },	{  52,152, 49, 0 },
	{  51,153, 48, 0 },	{  50,152, 48, 0 },	{   0,100, 49, 0 },	{   0,100, 49, 0 },
	{   0,100, 50, 0 },	{   0,  0,  0, 0 },	{   0,  0,  0, 0 },	{  50,  0, 70, 0 },
	{  50,  0, 70, 0 },	{   0,  0, 70, 0 },	{   0,  0, 70, 0 },	{   5,  0, 70, 0 },
	{   0,  0, 65, 0 },	{   0,  0, 65, 0 },	{   0,  0, 65, 0 },	{   0,  0,  0, 0 },
	{   0,  0,  0, 0 },	{ 210,210,210, 0 },	{ 190,190,190, 0 },	{ 167,167,167, 0 },
	{ 150,150,150, 0 },	{ 110,110,110, 0 },	{  85, 85, 85, 0 },	{  72, 72, 72, 0 },
	{  60, 60, 60, 0 },	{  40, 40, 40, 0 },	{  20, 20, 20, 0 },	{   0,  0,  0, 0 }
};					   



void CHFVExplorerListView::CreatePalette()
{
	UINT i;

	init_map_palette();

	if (m_Palette.m_hObject != NULL)
		m_Palette.DeleteObject();

	// memcpy( m_rgbPalette, rgbStd256, sizeof(RGBQUAD) * 256 );
	memcpy( m_rgbPalette, exact_mac256, sizeof(RGBQUAD) * 256 );

	
	LPLOGPALETTE lpLogPal;
	lpLogPal = (LPLOGPALETTE) new BYTE[sizeof(LOGPALETTE) + ((255) * sizeof(PALETTEENTRY))];
	lpLogPal->palVersion = 0x0300;
	lpLogPal->palNumEntries = 256;

	for (i = 0; i < 256; i++)
	{
		lpLogPal->palPalEntry[i].peRed = m_rgbPalette[i].rgbRed;
		lpLogPal->palPalEntry[i].peGreen = m_rgbPalette[i].rgbGreen;
		lpLogPal->palPalEntry[i].peBlue = m_rgbPalette[i].rgbBlue;
		lpLogPal->palPalEntry[i].peFlags = 0;
	}

	VERIFY( m_Palette.CreatePalette( lpLogPal ) );
	delete [] (BYTE *)lpLogPal;
}

CPalette* CHFVExplorerListView::GetPalette()
{
	if (m_Palette.m_hObject != NULL)
		return &m_Palette;
	else
		return NULL;
}

BOOL CHFVExplorerListView::OnQueryNewPalette() 
{
	if (GetPalette() == NULL)
		return FALSE;

	return FALSE;

	CClientDC dc(this);
	CPalette* pOldPalette = dc.SelectPalette(&m_Palette,
		GetCurrentMessage()->message == WM_PALETTECHANGED);

	UINT nChanged = dc.RealizePalette();
	dc.SelectPalette(pOldPalette, TRUE);

	if (nChanged == 0)
		return FALSE;

	Invalidate(FALSE);

	return TRUE;
}

void CHFVExplorerListView::OnPaletteChanged(CWnd* pFocusWnd) 
{
	if (pFocusWnd == this || IsChild(pFocusWnd))
		return;
	OnQueryNewPalette();
}

void CHFVExplorerListView::_OnPaletteChanged(CWnd* pFocusWnd) 
{
	OnPaletteChanged(pFocusWnd);
}

BOOL CHFVExplorerListView::_OnQueryNewPalette() 
{
	return(OnQueryNewPalette());
}

/*
void CHFVExplorerListView::OnPaint() 
{
	CPaintDC dc(this); // device context for painting

	LONG rstyle, style = GetWindowLong(m_cl->m_hWnd, GWL_STYLE);

	UINT nFormat;
	int icon_dim;

	rstyle = style & LVS_TYPEMASK;

	switch( rstyle) {
		case LVS_LIST:
			nFormat = DT_LEFT;
			rstyle = LVS_LIST;
			icon_dim = 16;
			break;
		case LVS_SMALLICON:
			nFormat = DT_LEFT;
			rstyle = LVS_SMALLICON;
			icon_dim = 16;
			break;
		case LVS_REPORT:
			nFormat = DT_LEFT;
			rstyle = LVS_REPORT;
			icon_dim = 16;
			break;
		case LVS_ICON:
			nFormat = DT_CENTER | DT_WORDBREAK;
			rstyle = LVS_ICON;
			icon_dim = 32;
			break;
	}

	HDC hdc = dc.GetSafeHdc();
	// SetBkColor( hdc, m_bkcolor );
	SetBkMode( hdc, TRANSPARENT );

	// ::SelectObject( hdc, m_hfont );

	int i, count = m_cl->GetItemCount();
	RECT r;

	CPalette* pOldPalette = dc.SelectPalette(&m_Palette,FALSE);
	UINT nChanged = dc.RealizePalette();
	
	for(i=0; i<count; i++) {
		DWORD_PTR dat;
		HICON hicon;
		CRect result_rect;
		CString text;

		dat = m_cl->GetItemData( i );
		hicon = get_lparam_icon32(dat);
		m_cl->GetItemRect( i, &r, LVIR_ICON );

		int x = r.left + ((r.right - r.left) - icon_dim) / 2;
		r.left = x;
		r.right = r.left + icon_dim;

		int y = r.top + ((r.bottom - r.top) - icon_dim) / 2;
		r.top = y;
		r.bottom = r.top + icon_dim;

		if(result_rect.IntersectRect(&dc.m_ps.rcPaint,&r)) {
			if(hicon) {
				dc.DrawIcon( r.left, r.top, hicon );
			} else {
				if(rstyle == LVS_ICON) {
					// m_pimagelist
				} else {
					// m_pimagelistSmall.Get
				}
			}
		}

		m_cl->GetItemRect( i, &r, LVIR_LABEL );
		if(result_rect.IntersectRect(&dc.m_ps.rcPaint,&r)) {
			if(rstyle == LVS_REPORT) {
				int j, w, x = 0;
				for(j=0; j<NCOLUMNS; j++) {
					w = m_cl->GetColumnWidth( j );
					text = m_cl->GetItemText( i, j );
					r.left = x;
					r.right = r.left + w;
					dc.DrawText( text, text.GetLength(), &r, nFormat );
					x += w;
				}
			} else {
				text = m_cl->GetItemText( i, 0 );
				dc.DrawText( text, text.GetLength(), &r, nFormat );
			}
		}
	}

	dc.SelectPalette(pOldPalette, TRUE);
}

void CHFVExplorerListView::OnItemchanged(NMHDR* pNMHDR, LRESULT* pResult) 
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;

	RECT r;
	int item;

	CDC *dc = m_cl->GetDC();
	if(pNMListView->uOldState != pNMListView->uNewState) {
		item = pNMListView->iItem;
		m_cl->GetItemRect( item, &r, LVIR_LABEL );
		dc->InvertRect( &r );
	}
	m_cl->ReleaseDC(dc);
	
	*pResult = 0;
}

typedef struct _NM_LISTVIEW
{
    NMHDR   hdr;
    int     iItem;
    int     iSubItem;
    UINT    uNewState;
    UINT    uOldState;
    UINT    uChanged;
    POINT   ptAction;
    LPARAM  lParam;
} NM_LISTVIEW, FAR *LPNM_LISTVIEW;

BOOL CHFVExplorerListView::OnEraseBkgnd(CDC* pDC) 
{
	CBrush cb;
	RECT r;
	cb.CreateSolidBrush( m_bkcolor );
	m_cl->GetClientRect(&r);
	pDC->FillRect( &r, &cb );
	return(0);
	// return CListView::OnEraseBkgnd(pDC);
}
*/

void CHFVExplorerListView::OnBegindrag(NMHDR* header,LRESULT* result)
{
    *result=0;
    if(m_dragging||StorageUi::busy()||StorageUi::state().sessionOpen)return;
    auto drag=std::make_unique<BrowserTransfers::Drag>(GetBrowserSession());
    if(drag->Empty())return;
    const auto event=reinterpret_cast<const NM_LISTVIEW*>(header);
    CPoint image;
    m_drag_list=m_cl->CreateDragImage(event->iItem,&image);
    if(!m_drag_list)return;
    if(!m_drag_list->BeginDrag(0,CPoint(0,0))){
        delete m_drag_list;
        m_drag_list=nullptr;
        return;
    }

    CaptureDragIcons();
    m_dragging=true;
    m_browserDrag=std::move(drag);
    m_sizeDelta=CPoint(event->ptAction)-image;
    // Use the displacement from the original pointer in list client coordinates.
    m_dragStart=event->ptAction;
    auto screen=m_dragStart-m_sizeDelta;
    m_cl->ClientToScreen(&screen);
    m_drag_list->DragEnter(nullptr,screen);
    m_cl->SetCapture();
}

void CHFVExplorerListView::CaptureDragIcons()
{
    m_dragIcons.clear();
    const auto style=m_cl->GetStyle()&LVS_TYPEMASK;
    if(style!=LVS_ICON&&style!=LVS_SMALLICON)return;
    for(int index=-1;(index=m_cl->GetNextItem(index,LVNI_SELECTED))>=0;){
        const auto row=BrowserRowAt(index);
        CPoint position;
        if(row&&!row->location.IsNone()&&m_cl->GetItemPosition(index,&position))
            m_dragIcons.push_back({index,row->location,position});
    }
}

void CHFVExplorerListView::PlaceDraggedIcons(CSize displacement)
{
    auto& session=GetBrowserSession();
    if(!session.ListingAvailable())return;
    std::vector<FileOperations::Request> requests;
    for(const auto& icon:m_dragIcons){
        const auto position=icon.position+displacement;
        m_cl->SendMessage(LVM_SETITEMPOSITION32,icon.index,LPARAM(&position));
        if(session.finderLayout&&icon.location.IsClassic()){
            const FileOperations::FinderPosition finder{
                short(NativeDpi::Scale(position.x,NativeDpi::Base,m_dpi)-FINDER_MAGIC_OFFSET+m_mac_origo.x),
                short(NativeDpi::Scale(position.y,NativeDpi::Base,m_dpi)-FINDER_MAGIC_OFFSET+m_mac_origo.y)};
            requests.emplace_back(FileOperations::EditClassicProperties{icon.location,finder});
        }
    }
    if(!requests.empty())BackgroundCopies::Start(std::move(requests),session.Owner(),true);
}

bool CHFVExplorerListView::IsParentRow(int index)const
{
    return m_parentRowData&&index>=0&&LPARAM(m_cl->GetItemData(index))==m_parentRowData;
}

#ifdef HFV_TESTING
void CHFVExplorerListView::PlaceSelectedIconsForTest(CSize displacement)
{
    CaptureDragIcons();
    PlaceDraggedIcons(displacement);
    m_dragIcons.clear();
}
#endif

void CHFVExplorerListView::set_cursor_by_type(int copy)
{
    if(const auto cursor=copy?m_copy_cursor:m_move_cursor)SetCursor(cursor);
}

void CHFVExplorerListView::UpdateDragModifiers()
{
    if(!m_browserDrag)return;
    CPoint screen;
    if(!GetCursorPos(&screen))return;
    const UINT keys=(GetKeyState(VK_CONTROL)<0?MK_CONTROL:0)|(GetKeyState(VK_SHIFT)<0?MK_SHIFT:0);
    m_browserDrag->Hover(screen,keys);
}

void CHFVExplorerListView::OnMouseMove(UINT flags,CPoint point)
{
    if(m_dragging){
        auto screen=point;
        m_cl->ClientToScreen(&screen);
        m_drag_list->DragMove(screen-m_sizeDelta);
        m_browserDrag->Hover(screen,flags);
    }
    CListView::OnMouseMove(flags,point);
}

void CHFVExplorerListView::FinishDragImage()
{
    m_dragging=false;
    if(m_drag_list){
        m_drag_list->DragLeave(nullptr);
        m_drag_list->EndDrag();
        delete m_drag_list;
        m_drag_list=nullptr;
    }
    if(::GetCapture()==GetSafeHwnd())::ReleaseCapture();
    SetCursor(LoadCursor(nullptr,IDC_ARROW));
}

void CHFVExplorerListView::OnButtonUp(CPoint point)
{
    if(!m_dragging)return;
    auto drag=std::move(m_browserDrag);
    FinishDragImage();
    auto screen=point;
    m_cl->ClientToScreen(&screen);
    const UINT keys=(GetKeyState(VK_CONTROL)<0?MK_CONTROL:0)|(GetKeyState(VK_SHIFT)<0?MK_SHIFT:0);
    if(!drag->Drop(screen,keys))PlaceDraggedIcons(point-m_dragStart);
    m_dragIcons.clear();
}

void CHFVExplorerListView::CancelBrowserDrag()
{
    if(m_dragging)FinishDragImage();
    m_browserDrag.reset();
    m_dragIcons.clear();
}
void CHFVExplorerListView::OnCancelMode(){CancelBrowserDrag();CListView::OnCancelMode();}
void CHFVExplorerListView::OnCaptureChanged(CWnd* next){if(m_dragging)CancelBrowserDrag();CListView::OnCaptureChanged(next);}
BOOL CHFVExplorerListView::PreTranslateMessage(MSG* message){
 if(m_dragging&&message->message==WM_KEYDOWN&&message->wParam==VK_ESCAPE){CancelBrowserDrag();return TRUE;}
 return CListView::PreTranslateMessage(message);
}
void CHFVExplorerListView::OnLButtonUp(UINT nFlags, CPoint point)
{
	OnButtonUp(point);
	CListView::OnLButtonUp(nFlags, point);
}

void CHFVExplorerListView::OnItemchanged(NMHDR* notification, LRESULT* result)
{
    const auto change=reinterpret_cast<const NMLISTVIEW*>(notification);
    if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED)) {
        if (m_browserSession && m_browserSession->Preview())
            m_browserSession->Preview()->RequestSelection();
    }
    *result=0;
}

void CHFVExplorerListView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	int inx, handled = 0;
	switch( nChar ) {
		case VK_ESCAPE:
			empty_clipboard();
			handled = 1;
			break;
		case VK_BACK:
			go_up( 0 );
			handled = 1;
			break;
		case VK_RETURN:
			inx = m_cl->GetNextItem( -1, LVNI_ALL | LVNI_FOCUSED );
			if(inx >= 0) {
				int ctrl = GetAsyncKeyState( VK_CONTROL ) & 0x80000000ul;
                if (ctrl) activate_list_item(inx, TRUE, FALSE);
                else RunBrowserItemCommand(ID_OWN_FILE_OPEN);
			}
			handled = 1;
			break;
		case VK_DELETE:
            RunBrowserItemCommand(ID_OWN_DELETE);
            handled = 1;
            break;
		case VK_F4:
			inx = m_cl->GetNextItem( -1, LVNI_SELECTED );
			if(inx >= 0) {
				map_selected_item();
			}
			handled = 1;
			break;
		case VK_TAB:
            if(auto tree=GetBrowserSession().Tree())tree->SetFocus();
			handled = 1;
			break;
		case VK_SHIFT:
		case VK_CONTROL:
			UpdateDragModifiers();
			handled = 1;
			break;
	}
	if(!handled) CListView::OnKeyDown(nChar, nRepCnt, nFlags);
}

void CHFVExplorerListView::OnEndlabeledit(NMHDR* pNMHDR, LRESULT* pResult) 
{
	LV_DISPINFO* pDispInfo = (LV_DISPINFO*)pNMHDR;

	*pResult = 0;

	int editing=m_editing_item;m_editing_item=-1;
	if(editing == pDispInfo->item.iItem) {
		if(pDispInfo->item.pszText) {
			if(!*pDispInfo->item.pszText) {
				strcpy(pDispInfo->item.pszText,m_editing_item_name);
			} else {
				if(strcmp(m_editing_item_name,pDispInfo->item.pszText) != 0) {

					if(do_rename( 
						editing,
						m_editing_item_name,
						pDispInfo->item.pszText, 1))
					{
						*pResult = BackgroundCopies::Enabled()?0:1;
					}
				}
			}
		}
	}
}

void CHFVExplorerListView::OnBeginlabeledit(NMHDR* pNMHDR, LRESULT* pResult) 
{
 if(GetBrowserSession().MacBrowser().Active()||!BrowserSelectionAllows(true)){*pResult=1;return;}
	LV_DISPINFO* pDispInfo = (LV_DISPINFO*)pNMHDR;

	m_editing_item = pDispInfo->item.iItem;
	lstrcpy( 
			m_editing_item_name, 
			m_cl->GetItemText( m_editing_item, 0 ) 
	);
	
	*pResult = 0;
}

void CHFVExplorerListView::OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	switch( nChar ) {
		case VK_SHIFT:
		case VK_CONTROL:
			UpdateDragModifiers();
			break;
	}
	CListView::OnKeyUp(nChar, nRepCnt, nFlags);
}

void CHFVExplorerListView::update_floppy_view( char *volpath )
{
	CHFVExplorerDoc *pdoc;

	pdoc = (CHFVExplorerDoc*)m_pDocument;
	pdoc->set_floppy_volume_dirty(volpath);
}

void CHFVExplorerListView::on_volume_changed( int volinx )
{
	/*
	CString currdir;
	int mac;

	currdir = ((CHFVExplorerDoc*)m_pDocument)->get_directory( 0, &mac );
	if(currdir != "") {
		((CHFVExplorerDoc*)m_pDocument)->my_chdir( currdir, mac );
	}
	*/
}

void CHFVExplorerListView::do_report_sort(void) {
 SortBrowserRows(m_sorted_by_column,m_sort_is_ascending!=FALSE);
}


void CHFVExplorerListView::OnColumnclick(NMHDR* pNMHDR, LRESULT* pResult) 
{
	NM_LISTVIEW* pNMListView = (NM_LISTVIEW*)pNMHDR;

	if(m_sorted_by_column == pNMListView->iSubItem) {
		// same column, reverse direction
		m_sort_is_ascending = !m_sort_is_ascending;
	} else {
		// column changed.
		m_sorted_by_column = pNMListView->iSubItem;
	}
	do_report_sort();
	*pResult = 0;
}

void CHFVExplorerListView::OnEditLabel() 
{
 if(GetBrowserSession().MacBrowser().Rename())return;
 if(!BrowserSelectionAllows(true))return;
	int nItem;
	nItem = m_cl->GetNextItem(-1,LVNI_SELECTED);
	if(nItem >= 0) {
        if(!GetBrowserSession().MacBrowser().RenameWindowsItem(nItem))m_cl->EditLabel(nItem);
	}
}

/*
typedef struct tagDRAWITEMSTRUCT {
    UINT        CtlType;
    UINT        CtlID;
    UINT        itemID;
    UINT        itemAction;
    UINT        itemState;
    HWND        hwndItem;
    HDC         hDC;
    RECT        rcItem;
    DWORD       itemData;
} DRAWITEMSTRUCT, NEAR *PDRAWITEMSTRUCT, FAR *LPDRAWITEMSTRUCT;

void CHFVExplorerListView::DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct) 
{
	int nItem;

	nItem = lpDrawItemStruct->itemID;

	if(nItem >= 0) {
		DWORD_PTR dat;
		HICON hicon;
		CRect result_rect;
		CString text;
		RECT r;

		int icon_dim = 32;

		dat = m_cl->GetItemData( nItem );
		hicon = get_lparam_icon32(dat);
		m_cl->GetItemRect( nItem, &r, LVIR_ICON );

		int x = r.left + ((r.right - r.left) - icon_dim) / 2;
		r.left = x;
		r.right = r.left + icon_dim;

		int y = r.top + ((r.bottom - r.top) - icon_dim) / 2;
		r.top = y;
		r.bottom = r.top + icon_dim;

		if(hicon) {
			::DrawIcon( lpDrawItemStruct->hDC, r.left, r.top, hicon );
		}
	}
}

	long lStyleOld;
	lStyleOld = GetWindowLong(m_cl->m_hWnd, GWL_STYLE);
	lStyleOld |= LVS_NOLABELWRAP;
	SetWindowLong(m_cl->m_hWnd, GWL_STYLE, lStyleOld);
*/

void CHFVExplorerListView::empty_clipboard( void )
{
 BrowserTransfers::Clear();
}

void CHFVExplorerListView::edit_cut_copy(BOOL cutting,BOOL append){GetBrowserSession().MacBrowser().Capture(cutting!=FALSE,append!=FALSE);}

void CHFVExplorerListView::edit_copy(void)
{
	edit_cut_copy( FALSE, FALSE );
}

void CHFVExplorerListView::edit_cut(void)
{
	edit_cut_copy( TRUE, FALSE );
}

void CHFVExplorerListView::edit_copy_append(void)
{
	edit_cut_copy( FALSE, TRUE );
}

void CHFVExplorerListView::edit_cut_append(void)
{
	edit_cut_copy( TRUE, TRUE );
}

void CHFVExplorerListView::edit_paste(void){GetBrowserSession().MacBrowser().Paste();}

void CHFVExplorerListView::edit_select_all(void)
{
	int inx = -1;

	while( (inx = m_cl->GetNextItem(inx,LVNI_ALL)) >= 0 ) {
		m_cl->SetItemState( inx, LVIS_SELECTED, LVIS_SELECTED );
	}
}
