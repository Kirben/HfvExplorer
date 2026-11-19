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

#include "stdafx.h"
#include "BrowserPresentation.h"
#include "LauncherWriteBack.h"
#include "CommandStorage.h"
#include "LegacyEngine.h"
#include "DialogClose.h"
#include "own_ids.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "HfsPlusDialogs.h"
#include "DeviceWait.h"
#include "MediaDiscovery.h"
#include "StorageJobDialog.h"
#include "FloppyWriteDialog.h"
#include "FloppyWriter.h"
#include "charset.h"
#include "FloppyImage.h"
#include "MediaControl.h"
#include "ReadOnlyCdBackend.h"
#include "CdImageDialog.h"
#include "CdBurnDialog.h"
#include "FindFilesDialog.h"
#include <errno.h>
#include "ImageSize.h"
#include "HFVPreviewView.h"
#include "resource.h"
#include "MainFrm.h"
#include "HFVExplorerListView.h"
#include "HFVExplorerTreeView.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorer.h"
#include "OptionsSheet.h"
#include "utils.h"
#include "hfs\libhfs\hfs.h"
#include "hfs\interface.h"
#include "AskNewVolume.h"
#include "AskVolumeToFloppy.h"
#include "utils.h"
#include "AskDump.h"
#include "special.h"

extern "C" {
#include "floppy.h"
}

#include <direct.h>
#include <dbt.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

#define VOLCHECK_TIMER_DELAY 1000
/////////////////////////////////////////////////////////////////////////////
// CMainFrame

#define CHILDMAGIC 0x12345678
UINT our_child_registration = RegisterWindowMessage("HFVRegisterChild");

IMPLEMENT_DYNCREATE(CMainFrame, CFrameWnd)

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
 ON_COMMAND(ID_FILE_OPEN, OnFileOpenImage)
 ON_COMMAND_RANGE(ID_FILE_MRU_FILE1, ID_FILE_MRU_FILE16, OnOpenRecentImage)
 ON_UPDATE_COMMAND_UI(ID_FILE_EJECT, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_FILE_RELOAD, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_CD_EJECT_RELOAD, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_CD_EJECT_EJECT, OnUpdatePhysicalMedia)


	//{{AFX_MSG_MAP(CMainFrame)
	ON_WM_CREATE()
	ON_COMMAND(ID_HFV_VIEW_DETAILS, OnViewDetails)
    ON_COMMAND(ID_VIEW_PREVIEW_PANE, OnViewPreviewPane)
    ON_UPDATE_COMMAND_UI(ID_VIEW_PREVIEW_PANE, OnUpdateViewPreviewPane)
	ON_COMMAND(ID_VIEW_LARGEICONS, OnViewLargeicons)
	ON_COMMAND(ID_HFV_VIEW_LIST, OnViewList)
	ON_COMMAND(ID_VIEW_SMALLICONS, OnViewSmallicons)
	ON_COMMAND(ID_VIEW_OPTIONS, OnViewOptions)
	ON_WM_ACTIVATEAPP()
 ON_WM_ACTIVATE()
	ON_WM_TIMER()
	ON_WM_QUERYNEWPALETTE()
	ON_WM_PALETTECHANGED()
	ON_WM_DROPFILES()
	ON_COMMAND(ID_FILE_NEWVOLUME, OnFileNewvolume)
 ON_COMMAND(ID_FILE_NEWHFSPLUS, OnFileNewHfsPlus)
 ON_UPDATE_COMMAND_UI_RANGE(ID_FILE_MAC_IMPORT,ID_FILE_MAC_IMPORT_RESOURCE,OnUpdateMacTransfer)
 ON_UPDATE_COMMAND_UI(ID_EDIT_CUT,OnUpdateMacEdit)
 ON_UPDATE_COMMAND_UI(ID_EDIT_PASTE,OnUpdateMacEdit)
	ON_COMMAND(ID_FILE_VOLUMEFROMFLOPPY, OnFileVolumefromfloppy)
	ON_COMMAND(ID_FILE_VOLUMETOFLOPPY, OnFileVolumetofloppy)

	ON_COMMAND(ID_TOOLS_DUMPCDDEBUG, OnToolsDumpcddebug)
	ON_COMMAND(ID_CD_EJECT_RELOAD, OnCdEjectReload)
	ON_COMMAND(ID_CD_EJECT_EJECT, OnCdEjectEject)
	ON_COMMAND(ID_HELP, OnHelp)
	ON_COMMAND(ID_VIEW_MAC_WINDOW, OnViewMacWindow)
	ON_COMMAND(ID_EDIT_COPY, OnEditCopy)
	ON_COMMAND(ID_EDIT_CUT, OnEditCut)
	ON_COMMAND(ID_EDIT_PASTE, OnEditPaste)
 ON_COMMAND(ID_EDIT_PASTE_HARD_LINK,OnPasteHardLink)
 ON_COMMAND(ID_FILE_SCAN_REMOVABLE,OnScanRemovable)
 ON_UPDATE_COMMAND_UI(ID_EDIT_PASTE_HARD_LINK,OnUpdatePasteHardLink)
	ON_COMMAND(ID_EDIT_COPY_APPEND, OnEditCopyAppend)
	ON_COMMAND(ID_EDIT_CUT_APPEND, OnEditCutAppend)
	ON_COMMAND(ID_EDIT_SELECT_ALL, OnEditSelectAll)
	ON_WM_QUERYOPEN()
	ON_WM_CLOSE()
	ON_COMMAND(ID_FILE_EJECT, OnFileEject)
	ON_COMMAND(ID_FILE_RELOAD, OnFileReload)
	ON_COMMAND(ID_TOOLS_BURN, OnToolsBurn)
 ON_COMMAND(ID_TOOLS_FIND, OnToolsFind)
	//}}AFX_MSG_MAP
	// Global help commands
	ON_COMMAND(ID_HELP_FINDER, OnHelp)
	// ON_COMMAND(ID_HELP, CFrameWnd::OnHelp)
	ON_COMMAND(ID_CONTEXT_HELP, OnHelp)
	ON_COMMAND(ID_DEFAULT_HELP, OnHelp)
END_MESSAGE_MAP()

static UINT indicators[] =
{
	ID_SEPARATOR,           // status line indicator
	ID_INDICATOR_CAPS,
	ID_INDICATOR_NUM,
	ID_INDICATOR_SCRL,
};

/////////////////////////////////////////////////////////////////////////////
// CMainFrame construction/destruction

CMainFrame::CMainFrame()
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
 m_timer=0;m_was_zoomed=FALSE;
	*m_waiting_emulator1 = 0;
	*m_waiting_emulator2 = 0;
	m_emulator_window_detected = 0;
	m_volcheck_enabled = TRUE;
}

CMainFrame::~CMainFrame()
{
}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();

	if (CFrameWnd::OnCreate(lpCreateStruct) == -1)
		return -1;
	
	if (!m_wndToolBar.Create(this) ||
		!m_wndToolBar.LoadToolBar(IDR_MAINFRAME))
	{
		TRACE0("Failed to create toolbar\n");
		return -1;      // fail to create
	}

	if (!m_wndStatusBar.Create(this) ||
		!m_wndStatusBar.SetIndicators(indicators,
		  sizeof(indicators)/sizeof(UINT)))
	{
		TRACE0("Failed to create status bar\n");
		return -1;      // fail to create
	}

    ApplyDpi(NativeDpi::Window(m_hWnd), nullptr);

	// TODO: Remove this if you don't want tool tips or a resizeable toolbar
	m_wndToolBar.SetBarStyle(m_wndToolBar.GetBarStyle() |
		CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC);

	// TODO: Delete these three lines if you don't want the toolbar to
	//  be dockable
	m_wndToolBar.EnableDocking(CBRS_ALIGN_ANY);
	EnableDocking(CBRS_ALIGN_ANY);
	DockControlBar(&m_wndToolBar);

 // Pane and command ownership belongs to this frame's BrowserSession.

	HFSIFACE_init( 0 ); // Only registered frames become legacy error owners.

	LoadWindowPosition();

	return 0;
}

void CMainFrame::OnUpdateFrameTitle(BOOL){
 if(m_browserSession)m_browserSession->UpdateWindowTitle();else if(IsWindow(m_hWnd))::SetWindowTextW(m_hWnd,L"HFVExplorer");
}
BOOL CMainFrame::PreCreateWindow(CREATESTRUCT& cs)
{
 cs.style&=~FWS_ADDTOTITLE; // Session captions replace MFC's Untitled document name.
	// TODO: Modify the Window class or styles here by modifying
	//  the CREATESTRUCT cs

	return CFrameWnd::PreCreateWindow(cs);
}

/////////////////////////////////////////////////////////////////////////////
// CMainFrame diagnostics

#ifdef _DEBUG
void CMainFrame::AssertValid() const
{
	CFrameWnd::AssertValid();
}

void CMainFrame::Dump(CDumpContext& dc) const
{
	CFrameWnd::Dump(dc);
}

#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CMainFrame message handlers

BOOL CMainFrame::OnCreateClient(LPCREATESTRUCT lpcs, CCreateContext* pContext)
{
	if(!m_wndSplitter.CreateStatic( this, 1, 2, WS_CHILD | WS_VISIBLE ))
	{
		TRACE0("Failed to create split bar ");
		return FALSE;
	}

	if (!m_wndSplitter.CreateView(0, PANE_TREE,
		RUNTIME_CLASS(HFVExplorerTreeView), CSize(200, 0), pContext))
	{
		TRACE0("Failed to create first pane\n");
		return FALSE;
	}

	if (!m_wndSplitter2.CreateStatic(
		&m_wndSplitter, 2, 1,
		WS_CHILD | WS_VISIBLE | WS_BORDER,
		m_wndSplitter.IdFromRowCol(0, 1) ))
	{
		TRACE0("Failed to create nested splitter\n");
		return FALSE;
	}

	if (!m_wndSplitter2.CreateView(0, 0,
		RUNTIME_CLASS(CHFVExplorerListView), CSize(0, 0), pContext))
	{
		TRACE0("Failed to create second pane\n");
		return FALSE;
	}

	if (!m_wndSplitter2.CreateView(1, 0,
		RUNTIME_CLASS(CHFVPreviewView), CSize(0, 0), pContext))
	{
		TRACE0("Failed to create third pane\n");
		return FALSE;
	}

	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

 auto tree=(HFVExplorerTreeView*)m_wndSplitter.GetPane(0,0);
 auto list=(CHFVExplorerListView*)m_wndSplitter2.GetPane(0,0);
 auto preview=(CHFVPreviewView*)m_wndSplitter2.GetPane(1,0);
 m_browserSession=&static_cast<CHFVExplorerDoc*>(pContext->m_pCurrentDoc)->GetBrowserSession();
 m_browserSession->Attach(this,tree,list,preview,&m_wndStatusBar);

 m_wndSplitter2.ShowPreview(*preview,false);
 tree->Initialize();list->Initialize();list->set_style(LVS_ICON);list->FillList();

	m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );

	return TRUE;
	// return CFrameWnd::OnCreateClient(lpcs, pContext);
}

void BrowserSplitter::ApplyDpi(UINT from, UINT to)
{
    if (!m_pColInfo || !m_pRowInfo) return;
    auto& first = m_nCols > 1 ? m_pColInfo[0] : m_pRowInfo[0];
    first.nIdealSize = firstPane_.Resize(first.nIdealSize, from, to);
    m_cxSplitter = m_cySplitter = NativeDpi::Scale(7, to);
    m_cxSplitterGap = m_cySplitterGap = m_cxSplitter;
    m_cxBorder = m_cyBorder = NativeDpi::Scale(2, to);
    dpi_ = to;
}

void BrowserToolbar::ApplyDpi(UINT dpi)
{
    const auto name = MAKEINTRESOURCE(IDR_MAINFRAME);
    const auto instance = AfxFindResourceHandle(name, RT_BITMAP);
    const auto resource = FindResource(instance, name, RT_BITMAP);
    CBitmap source;
    if (!resource || !source.Attach(AfxLoadSysColorBitmap(instance, resource))) return;
    BITMAP bitmap{};
    source.GetBitmap(&bitmap);
    constexpr int imageWidth = 16, imageHeight = 15;
    const int width = NativeDpi::Scale(imageWidth, dpi);
    const int height = NativeDpi::Scale(imageHeight, dpi);
    const int count = bitmap.bmWidth / imageWidth;

    CClientDC client(this);
    CDC input, output;
    CBitmap scaled;
    if (!input.CreateCompatibleDC(&client) || !output.CreateCompatibleDC(&client) ||
        !scaled.CreateCompatibleBitmap(&client, count*width, height)) return;
    const auto in = input.SelectObject(&source);
    const auto out = output.SelectObject(&scaled);
    output.SetStretchBltMode(COLORONCOLOR);
    const BOOL resized = output.StretchBlt(0, 0, count*width, height, &input,
        0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
    input.SelectObject(in);
    output.SelectObject(out);
    if (!resized) return;

    const CSize oldButton = m_sizeButton, oldImage = m_sizeImage;
    SetSizes(CSize(NativeDpi::Scale(23, dpi), NativeDpi::Scale(21, dpi)),
        CSize(width, height));
    if (SetBitmap(HBITMAP(scaled.GetSafeHandle()))) {
        scaled.Detach();
        dpi_ = dpi;
    } else {
        SetSizes(oldButton, oldImage);
    }
}

LRESULT BrowserToolbar::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SYSCOLORCHANGE && m_hInstImageWell == nullptr)
        ApplyDpi(dpi_);
    return CToolBar::WindowProc(message, wParam, lParam);
}

void CMainFrame::ApplyDpi(UINT dpi, const RECT* suggested)
{
    if (!dpi) return;
    const UINT previous = m_dpi;
    m_dpi = dpi;
    if (m_browserSession && previous != dpi) {
        m_wndSplitter.ApplyDpi(previous, dpi);
        m_wndSplitter2.ApplyDpi(previous, dpi);
        m_browserSession->List()->ApplyDpi(dpi);
        m_browserSession->Tree()->ApplyDpi(dpi);
        m_browserSession->Preview()->ApplyDpi(dpi);
        if (m_wndToolBar.GetSafeHwnd()) m_wndToolBar.ApplyDpi(dpi);
    }
    if (m_wndStatusBar.GetSafeHwnd()) {
        auto& app = *static_cast<CHFVExplorerApp*>(AfxGetApp());
        m_wndStatusBar.SendMessage(WM_SETFONT, WPARAM(app.get_font(dpi)), TRUE);
        if (previous != dpi) {
            static_assert(ARRAYSIZE(indicators) == ARRAYSIZE(m_statusWidths));
            for (int i = 0; i < ARRAYSIZE(indicators); ++i) {
                UINT id, style;
                int width;
                m_wndStatusBar.GetPaneInfo(i, id, style, width);
                m_wndStatusBar.SetPaneInfo(i, id, style, m_statusWidths[i].Resize(width, previous, dpi));
            }
        }
    }
    if (suggested) {
        SetWindowPos(nullptr, suggested->left, suggested->top,
            suggested->right-suggested->left, suggested->bottom-suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RecalcLayout();
}

void PreviewSplitter::ShowPreview(CWnd& pane, bool visible)
{
    // Keep the second static pane alive, but omit its row and splitter bar
    // from layout when disabled.
    m_nRows=visible ? 2 : 1;
    if(visible) {
        CRect rect;
        GetClientRect(rect);
        const int preferred=NativeDpi::Scale(rect.Width()<NativeDpi::Scale(510,dpi_) ? 230 : 168,dpi_);
        SetRowInfo(0,(std::max)(NativeDpi::Scale(64,dpi_),rect.Height()-(std::min)(preferred,rect.Height()/2)),0);
        SetRowInfo(1,0,0);
    }
    pane.ShowWindow(visible ? SW_SHOWNA : SW_HIDE);
    RecalcLayout();
}

void PreviewSplitter::RecalcLayout()
{
    if(m_nRows==2) {
        CRect rect;
        GetClientRect(rect);
        auto& first=m_pRowInfo[0].nIdealSize;
        first=(std::min)(first,(std::max)(NativeDpi::Scale(24,dpi_),rect.Height()-(std::min)(NativeDpi::Scale(116,dpi_),rect.Height()/2)));
    }
    CSplitterWnd::RecalcLayout();
}

void CMainFrame::OnViewPreviewPane()
{
    auto preview=m_browserSession ? m_browserSession->Preview() : nullptr;
    if(!preview)return;
    const bool show=!preview->Enabled();
    if(!show) {
        const HWND focus=::GetFocus();
        if(GetActiveView()==preview || focus==preview->GetSafeHwnd() || ::IsChild(preview->GetSafeHwnd(),focus)) {
            SetActiveView(m_browserSession->List());
            m_browserSession->List()->SetFocus();
        }
    }
    preview->EnablePreview(show);
    m_wndSplitter2.ShowPreview(*preview,show);
}

void CMainFrame::OnUpdateViewPreviewPane(CCmdUI* ui)
{
    auto preview=m_browserSession ? m_browserSession->Preview() : nullptr;
    ui->Enable(preview!=nullptr);
    ui->SetCheck(preview && preview->Enabled());
}

void CMainFrame::OnViewDetails() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (GetBrowserSession().finderLayout == TRUE);
	GetBrowserSession().finderLayout = FALSE;pApp->m_mac_window_mode=FALSE; // Default for future sessions.
	GetBrowserSession().List()->set_style( LVS_REPORT );
	if(changed_mode) GetBrowserSession().Tree()->mode_changed();
}

void CMainFrame::OnViewLargeicons() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (GetBrowserSession().finderLayout == TRUE);
	GetBrowserSession().finderLayout = FALSE;pApp->m_mac_window_mode=FALSE; // Default for future sessions.
	GetBrowserSession().List()->set_style( LVS_ICON );
	if(changed_mode) GetBrowserSession().Tree()->mode_changed();
}

void CMainFrame::OnViewList() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (GetBrowserSession().finderLayout == TRUE);
	GetBrowserSession().finderLayout = FALSE;pApp->m_mac_window_mode=FALSE; // Default for future sessions.
	GetBrowserSession().List()->set_style( LVS_LIST );
	if(changed_mode) GetBrowserSession().Tree()->mode_changed();
}

void CMainFrame::OnViewSmallicons() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (GetBrowserSession().finderLayout == TRUE);
	GetBrowserSession().finderLayout = FALSE;pApp->m_mac_window_mode=FALSE; // Default for future sessions.
	GetBrowserSession().List()->set_style( LVS_SMALLICON );
	if(changed_mode) GetBrowserSession().Tree()->mode_changed();
}

void CMainFrame::OnViewMacWindow() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (GetBrowserSession().finderLayout == FALSE);
	GetBrowserSession().finderLayout = TRUE;pApp->m_mac_window_mode=TRUE; // Default for future sessions.
	GetBrowserSession().List()->set_style( LVS_ICON );
	if(changed_mode) GetBrowserSession().Tree()->mode_changed();
}

void CMainFrame::OnProgramProperties() 
{
	static int page = 0;
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	int save_point_size = point_size;
	char save_font_name[FONTNAMESIZE];
	int save_is_font_sjis = is_font_sjis; /* noda */

	lstrcpy( save_font_name, font_name );

	COptionsSheet propSheet( "HFVExplorer Options", this, page );

	propSheet.m_Page1.m_mountall = pApp->m_mountall;
	propSheet.m_Page1.m_dosicons = pApp->m_dosicons;
	propSheet.m_Page1.m_fat_icon_time_limit = pApp->m_fat_icon_time_limit;

	propSheet.m_Page1.m_startup_floppies = pApp->m_startup_floppies;
	propSheet.m_Page1.m_startup_cds = pApp->m_startup_cds;
	propSheet.m_Page1.m_startup_hds = pApp->m_startup_hds;

	propSheet.m_Page1.m_psize.Format( "%d", point_size );
	propSheet.m_Page1.m_font = CString(font_name);
	propSheet.m_Page1.m_is_font_sjis = pApp->m_is_font_sjis;/* noda*/

	propSheet.m_Page2.m_copyout_mode = pApp->m_copyout_mode;
	propSheet.m_Page2.m_copyin_mode = pApp->m_copyin_mode;
	propSheet.m_Page2.m_copyin_appledouble = pApp->m_copyin_appledouble;

	propSheet.m_Page3.m_show_invisible_mac = pApp->m_show_invisible_mac;
	propSheet.m_Page3.m_show_invisible_fat = pApp->m_show_invisible_fat;
	propSheet.m_Page3.m_protect_locked_mac = pApp->m_protect_locked_mac;
	propSheet.m_Page3.m_protect_locked_fat = pApp->m_protect_locked_fat;

	propSheet.m_Page4.m_default_emulator = pApp->m_default_emulator;
	propSheet.m_Page4.m_link_name_style = pApp->m_link_name_style;
	
	propSheet.m_Page5.m_executor_dos_path = pApp->m_executor_dos_path;
	
	propSheet.m_Page6.m_executor_win32_path = pApp->m_executor_win32_path;
	
	propSheet.m_Page7.m_vmac_path = pApp->m_vmac_path;
	propSheet.m_Page7.m_vmac_startup_folder_name = pApp->m_vmac_startup_folder_name;
	propSheet.m_Page7.m_vmac_system_file_path = pApp->m_vmac_system_file_path;
	propSheet.m_Page7.m_launch_method = pApp->m_launch_method;
	propSheet.m_Page7.m_use_remover = pApp->m_use_remover;

	propSheet.m_Page8.m_shapeshifter_path = pApp->m_shapeshifter_path;
	propSheet.m_Page8.m_shapeshifter_startup_folder_name = pApp->m_shapeshifter_startup_folder_name;
	propSheet.m_Page8.m_shapeshifter_system_file_path = pApp->m_shapeshifter_system_file_path;
	propSheet.m_Page8.m_shapeshifter_preferences_file_path = pApp->m_shapeshifter_preferences_file_path;
	propSheet.m_Page8.m_shapeshifter_amiga_volume_name = pApp->m_shapeshifter_amiga_volume_name;
	propSheet.m_Page8.m_shapeshifter_use_remover = pApp->m_shapeshifter_use_remover;

	propSheet.m_Page9.m_fusion_startup_folder_name = pApp->m_fusion_startup_folder_name;
	propSheet.m_Page9.m_fusion_system_file_path = pApp->m_fusion_system_file_path;
	propSheet.m_Page9.m_fusion_batch_path = pApp->m_fusion_batch_path;
	propSheet.m_Page9.m_fusion_use_remover = pApp->m_fusion_use_remover;


	if (propSheet.DoModal() == IDOK) {
		page = propSheet.m_last_active_page;
		pApp->m_mountall = propSheet.m_Page1.m_mountall;
		pApp->m_dosicons = propSheet.m_Page1.m_dosicons;
		pApp->m_fat_icon_time_limit = propSheet.m_Page1.m_fat_icon_time_limit;
		pApp->m_startup_floppies = propSheet.m_Page1.m_startup_floppies;
		pApp->m_startup_cds = propSheet.m_Page1.m_startup_cds;
		pApp->m_startup_hds = propSheet.m_Page1.m_startup_hds;

		point_size = atoi( (LPCSTR)propSheet.m_Page1.m_psize );
		lstrcpy( font_name, (LPCSTR)propSheet.m_Page1.m_font );
		pApp->m_is_font_sjis = is_font_sjis = propSheet.m_Page1.m_is_font_sjis; /* noda */

		pApp->m_copyout_mode = propSheet.m_Page2.m_copyout_mode;
		pApp->m_copyin_mode = propSheet.m_Page2.m_copyin_mode;
		pApp->m_copyin_appledouble = propSheet.m_Page2.m_copyin_appledouble;

		pApp->m_show_invisible_mac = propSheet.m_Page3.m_show_invisible_mac;
		pApp->m_show_invisible_fat = propSheet.m_Page3.m_show_invisible_fat;
		pApp->m_protect_locked_mac = propSheet.m_Page3.m_protect_locked_mac;
		pApp->m_protect_locked_fat = propSheet.m_Page3.m_protect_locked_fat;

		pApp->m_default_emulator = propSheet.m_Page4.m_default_emulator;
		pApp->m_link_name_style = propSheet.m_Page4.m_link_name_style;

		pApp->m_executor_dos_path = propSheet.m_Page5.m_executor_dos_path;

		pApp->m_executor_win32_path = propSheet.m_Page6.m_executor_win32_path;

		pApp->m_vmac_path = propSheet.m_Page7.m_vmac_path;
		pApp->m_vmac_startup_folder_name = propSheet.m_Page7.m_vmac_startup_folder_name;
		pApp->m_vmac_system_file_path = propSheet.m_Page7.m_vmac_system_file_path;
		pApp->m_launch_method = propSheet.m_Page7.m_launch_method;
		pApp->m_use_remover = propSheet.m_Page7.m_use_remover;

		pApp->m_shapeshifter_path = propSheet.m_Page8.m_shapeshifter_path;
		pApp->m_shapeshifter_startup_folder_name = propSheet.m_Page8.m_shapeshifter_startup_folder_name;
		pApp->m_shapeshifter_system_file_path = propSheet.m_Page8.m_shapeshifter_system_file_path;
		pApp->m_shapeshifter_preferences_file_path = propSheet.m_Page8.m_shapeshifter_preferences_file_path;
		pApp->m_shapeshifter_amiga_volume_name = propSheet.m_Page8.m_shapeshifter_amiga_volume_name;
		pApp->m_shapeshifter_use_remover = propSheet.m_Page8.m_shapeshifter_use_remover;

		pApp->m_fusion_startup_folder_name = propSheet.m_Page9.m_fusion_startup_folder_name;
		pApp->m_fusion_system_file_path = propSheet.m_Page9.m_fusion_system_file_path;
		pApp->m_fusion_batch_path = propSheet.m_Page9.m_fusion_batch_path;
		pApp->m_fusion_use_remover = propSheet.m_Page9.m_fusion_use_remover;


		HFSIFACE_set_copy_modes( 
			pApp->m_copyout_mode,
			pApp->m_copyin_mode,
			pApp->m_copyin_appledouble
		);

		GetBrowserSession().Document()->update_invisible_attributes();

		if(save_point_size != point_size || stricmp(font_name,save_font_name) != 0 || save_is_font_sjis != is_font_sjis) { /* noda */
			AfxMessageBox( "You need to restart the program for the font change to take effect." );
		}
		// pApp->set_font( font_name, point_size );

		pApp->SaveSettings();

	}
}

void CMainFrame::OnViewOptions() 
{
	OnProgramProperties();
}

void CMainFrame::OnActivate(UINT state,CWnd* other,BOOL minimized){
 CFrameWnd::OnActivate(state,other,minimized);
 if(state!=WA_INACTIVE)static_cast<CHFVExplorerApp*>(AfxGetApp())->ActivateBrowser(this);
}
bool CMainFrame::CommandAllowed(UINT id)const{
 if(!m_browserSession)return true;
 if(id==ID_FILE_EJECT||id==ID_FILE_RELOAD||id==ID_CD_EJECT_EJECT||id==ID_CD_EJECT_RELOAD){auto target=SelectedMediaControlTarget();return CanControlMedia(target)&&VolumeCoord::Available({{VolumeCoord::DeviceKey(target.drive),VolumeCoord::Mode::Write}});}
 if(id==ID_VIEW_MAC_WINDOW||id==ID_TOOLS_FIND){auto key=GetBrowserSession().MacBrowser().SelectedResourceKey();return key.empty()||VolumeCoord::Available({{key,VolumeCoord::Mode::Read}});}
 return true;
}
BOOL CMainFrame::OnCmdMsg(UINT id,int code,void* extra,AFX_CMDHANDLERINFO* info){
 if(!info&&!CommandAllowed(id)){if(code==CN_UPDATE_COMMAND_UI)static_cast<CCmdUI*>(extra)->Enable(FALSE);return TRUE;}

 if (!info && m_browserSession && m_browserSession->List()) {
  auto& list = *m_browserSession->List();
  if (code == CN_UPDATE_COMMAND_UI) {
   if (const auto enabled = list.BrowserItemCommandEnabled(id)) {
    static_cast<CCmdUI*>(extra)->Enable(*enabled);
    return TRUE;
   }
  }
 }
 if(code==CN_COMMAND&&!info){
  struct ErrorOwner{
   HWND previous;
   ~ErrorOwner(){HFSIFACE_exchange_error_window(previous);}
  } owner{HFSIFACE_exchange_error_window(GetSafeHwnd())};
  if (m_browserSession && m_browserSession->List() && m_browserSession->List()->RunBrowserItemCommand(id))
      return TRUE;
  // Handle image opens before document and application command fallback.
  if(id==ID_FILE_OPEN){OnFileOpenImage();return TRUE;}
  if(id>=ID_FILE_MRU_FILE1&&id<=ID_FILE_MRU_FILE16){OnOpenRecentImage(id);return TRUE;}
  return CFrameWnd::OnCmdMsg(id,code,extra,info);
 }
 return CFrameWnd::OnCmdMsg(id,code,extra,info);
}
void CMainFrame::OnFileOpenImage()
{
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return;
 auto& session=GetBrowserSession();
 CFileDialog dialog(TRUE,"DSK",nullptr,OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
    "Mac disk images|*.dsk;*.hf*;*.img;*.image;*.mfs;*.dc42|All Files|*.*||",session.Owner());
 if(dialog.DoModal()==IDOK)session.Document()->OnOpenDocument(dialog.GetPathName());
}
void CMainFrame::OnOpenRecentImage(UINT command)
{
 static_cast<CHFVExplorerApp*>(AfxGetApp())->OpenRecentImage(command,GetBrowserSession());
}
void CMainFrame::OnActivateApp(BOOL bActive, DWORD hTask)
{
	CFrameWnd::OnActivateApp(bActive, hTask);
	
	if(bActive && m_browserSession && GetBrowserSession().Document() && !StorageUi::busy() && !StorageUi::state().sessionOpen) {
		// check HFV files for changes
		CHFVExplorerApp	*pApp;
		pApp = (CHFVExplorerApp *)AfxGetApp();
		GetBrowserSession().Document()->check_hfv_file_times( 0 );
		// if(!m_timer) m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );
		m_volcheck_enabled = TRUE;
	} else {
		// if(m_timer) KillTimer(m_timer);
		// m_timer = 0;
		m_volcheck_enabled = FALSE;
	}
}

HWND CMainFrame::FindEmuWindow(void) 
{
	HWND w;
	w = ::FindWindow( m_waiting_emulator1, NULL );
	if(w) return(w);
	w = ::FindWindow( m_waiting_emulator2, NULL );
	if(w) return(w);
	return(0);
}

BOOL CALLBACK  GetProcessesProc( HWND hwnd, LPARAM lParam )
{
	DWORD pid;
	CDWordArray *plist = (CDWordArray *)lParam;

	GetWindowThreadProcessId( hwnd, &pid );
	plist->Add(pid);
	return(TRUE);
}

static void get_process_list( CDWordArray & plist )
{
	plist.RemoveAll();
	EnumWindows( GetProcessesProc, (LPARAM)&plist );
}

// To enumerate all of the processes on the system, you can query the Registry using RegQueryValueEx() with key HKEY_PERFORMANCE_DATA, and the Registry database index associated with the database string "Process". 

// hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
// TerminateProcess(hProcess, 0);

void CMainFrame::OnTimer(UINT_PTR nIDEvent)
{
 if(nIDEvent==9202){
  if(!BackgroundCopies::Active()&&!StorageUi::busy()&&!StorageUi::state().sessionOpen){
   auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
   for(auto frame:app->m_browsers)frame->KillTimer(9202);
   app->OnExitBrowsers();
  }
  return;
 }
 if(nIDEvent==9201){if(!BackgroundCopies::Active()||static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers.size()>1){KillTimer(9201);PostMessage(WM_CLOSE);}return;}
 if(HfvDeviceWait::busy()||StorageUi::state().sessionOpen)return;

	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	if(m_handlingTimer)return;

	m_handlingTimer=true;
 struct TimerGuard{bool& active;~TimerGuard(){active=false;}} guard{m_handlingTimer};

 if(!StorageUi::busy()){
  for(size_t i=m_launchedDocuments.size();i>0;--i){
   auto& item=m_launchedDocuments[i-1];
   if(!item->Ready())continue;
   auto completed=std::move(item);
   m_launchedDocuments.erase(m_launchedDocuments.begin()+i-1);
   completed->Complete(GetBrowserSession());
   break;
  }
 }

	if(*m_waiting_emulator1) {
		int stop_wait = 0;
		if(GetTickCount() > m_start_watch_emulator) {
			if(!FindEmuWindow()) {
				stop_wait = 1;
			} else if(!m_emulator_window_detected) {
				m_emulator_window_detected = 1;
			}
		} else if(m_emulator_window_detected) {
			if(!FindEmuWindow()) {
				stop_wait = 1;
			}
		} else {
			if(FindEmuWindow()) {
				m_emulator_window_detected = 1;
			}
		}
		if(stop_wait) {
			*m_waiting_emulator1 = 0;
			*m_waiting_emulator2 = 0;
			if(m_was_zoomed) {
				ShowWindow( SW_RESTORE );
				ShowWindow( SW_SHOWMAXIMIZED );
			} else {
				ShowWindow( SW_RESTORE );
			}
			SetForegroundWindow();
			UpdateWindow();
			m_was_zoomed = 0;
			m_start_watch_emulator = 0;
		}
	}

	if(pApp->m_timer_enabled && m_volcheck_enabled) {
		if(GetBrowserSession().Document() && GetBrowserSession().List() && GetBrowserSession().Tree()) {
			// if(m_timer) KillTimer(m_timer);
			// m_timer = 0;
			m_volcheck_enabled = FALSE;
			GetBrowserSession().Document()->check_hfv_file_times( 0 );
			m_volcheck_enabled = TRUE;
			// if(!m_timer) m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );
		}
	}


	CFrameWnd::OnTimer(nIDEvent);
}

void CMainFrame::WaitEmulator( char *name1, char *name2 ) 
{
	strcpy( m_waiting_emulator1, name1 );
	strcpy( m_waiting_emulator2, name2 );
	m_was_zoomed = IsZoomed();
	m_start_watch_emulator = GetTickCount() + 10000ul;
	m_emulator_window_detected = 0;
	ShowWindow( SW_MINIMIZE );
}

void CMainFrame::TrackLaunchedDocument(std::unique_ptr<LauncherWriteBack> document)
{
 m_launchedDocuments.push_back(std::move(document));
}

BOOL CMainFrame::DestroyWindow() 
{
	SaveWindowPosition();
	if(m_timer) KillTimer(m_timer);
	m_timer = 0;
	m_volcheck_enabled = FALSE;
	
 static_cast<CHFVExplorerApp*>(AfxGetApp())->ForgetBrowser(this);
	return CFrameWnd::DestroyWindow();
}

void CMainFrame::OnPaletteChanged(CWnd* pFocusWnd) 
{
	CFrameWnd::OnPaletteChanged(pFocusWnd);
	
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	if(m_browserSession&&GetBrowserSession().List())GetBrowserSession().List()->_OnPaletteChanged(pFocusWnd);
}

BOOL CMainFrame::OnQueryNewPalette() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	return m_browserSession&&GetBrowserSession().List()?GetBrowserSession().List()->_OnQueryNewPalette():FALSE;
	// return CFrameWnd::OnQueryNewPalette();
}

void CMainFrame::OnDropFiles(HDROP drop)
{
    std::vector<std::wstring> images;
    const auto count=DragQueryFileW(drop,0xffffffff,nullptr,0);
    for(UINT index=0;index<count;++index){
        const auto size=DragQueryFileW(drop,index,nullptr,0);
        std::wstring path(size+1,0);
        if(!size||DragQueryFileW(drop,index,path.data(),size+1)!=size)continue;
        path.resize(size);
        const auto dot=path.find_last_of(L'.');
        if(dot==path.npos)continue;
        const auto extension=path.substr(dot);
        if(!_wcsicmp(extension.c_str(),L".dsk")||!_wcsnicmp(extension.c_str(),L".hf",3)||
           !_wcsicmp(extension.c_str(),L".img")||!_wcsicmp(extension.c_str(),L".image")||
           !_wcsicmp(extension.c_str(),L".mfs")||!_wcsicmp(extension.c_str(),L".dc42"))
            images.push_back(std::move(path));
    }
    DragFinish(drop);
    GetBrowserSession().MacBrowser().OpenImages(std::move(images));
}

void CMainFrame::OnChildRegistration(WPARAM wParam, LPARAM lParam) 
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->add_child( (HWND)lParam );
}

LRESULT CMainFrame::WindowProc(UINT message, WPARAM wParam, LPARAM lParam) 
{
 if(message==NativeDpi::Changed){
  ApplyDpi(LOWORD(wParam),reinterpret_cast<const RECT*>(lParam));
  return 0;
 }
 if((message==WM_COMMAND||message==WM_CLOSE)&&(StorageUi::busy()||StorageUi::state().sessionOpen))return 0;
 if(message==WM_SYSCOMMAND&&(wParam&0xfff0)==SC_CLOSE){
  if(StorageUi::busy()||StorageUi::state().sessionOpen)return 0;
  auto kind=!BackgroundCopies::Active()?DialogClose::Kind::Browser:static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers.size()<=1?DialogClose::Kind::LastBrowser:DialogClose::Kind::BackgroundBrowser;
  if(!DialogClose::Confirm(m_hWnd,kind))return 0;
 }
 if(message==WM_COMMAND&&!StorageUi::busy()){
  StorageAction storageAction;
  return CFrameWnd::WindowProc(message,wParam,lParam);
 }
 if(message==WM_DEVICECHANGE&&m_browserSession&&lParam&&
    (wParam==DBT_DEVICEARRIVAL||wParam==DBT_DEVICEREMOVECOMPLETE)){
  const auto* notice=reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lParam);
  if(notice->dbcv_devicetype==DBT_DEVTYP_VOLUME&&notice->dbcv_size>=sizeof(*notice)){
   const auto& browsers=static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers;
   if(browsers.empty()||browsers.front()==this)MediaDiscovery::Changed(notice->dbcv_unitmask);
   m_browserSession->NavigationMediaChanged(notice->dbcv_unitmask);
   auto& browser=m_browserSession->MacBrowser();
   browser.QueueMediaChanges(notice->dbcv_unitmask,wParam==DBT_DEVICEARRIVAL?notice->dbcv_unitmask:0);
   return TRUE;
  }
 }
 if(HfvDeviceWait::busy()&&(message==WM_COMMAND||message==WM_CLOSE))return 0;
 if(message==our_child_registration)OnChildRegistration(wParam,lParam);
	return CFrameWnd::WindowProc(message, wParam, lParam);
}

int ask_new_volume(char* volpath,char* vname,hfv_size_t* psize,int* normal,int* floppyDrive)
{
 CAskNewVolume dlg;dlg.m_allowFloppies=floppyDrive && !*volpath;
 if(floppyDrive)*floppyDrive=-1;
 dlg.m_volname=vname;dlg.m_volpath=volpath;dlg.m_volsize_ex="1440 kB";
 if(dlg.DoModal()!=IDOK)return 0;
 int drive=CAskNewVolume::FloppySelection(dlg.m_volpath);
 if(drive>=0){
  if(!dlg.m_allowFloppies||!floppyDrive)return 0;
  *floppyDrive=drive;*normal=0;*psize=1474560;*volpath=0;lstrcpy(vname,dlg.m_volname);return 1;
 }
 if(!ParseImageSize(dlg.m_volsize_ex,psize)){
  AfxMessageBox("Invalid image size. Use a whole number with kB, MB or GB units, aligned to 512 bytes.");return 0;
 }
 lstrcpy(vname,dlg.m_volname);lstrcpy(volpath,dlg.m_volpath);*normal=1;return 1;
}

int write_floppy( int drive, char *path )
{
	int i, LBA = 0;
	HANDLE hf;
	char *buf = 0;
	DWORD bytes_read;
	HANDLE hfloppy;
	int C0, H0, S0, nSec;
	int chunk, nIter, ok = 0;

	hf = CreateFile( path, GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL );	
	if( hf == INVALID_HANDLE_VALUE ) {
		AfxMessageBox( "Could not open file " + CString(path) );
		return(0);
	}
	hfloppy = floppy_init( drive, TRUE );
	if(hfloppy) {
		floppy_get_geometry( drive, &C0, &H0, &S0, &nSec );
		chunk = S0 * nSec;
		nIter = C0 * H0;
		// buf = (char *)malloc( chunk );
		buf = (char *)VirtualAlloc( 
				NULL, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
		if(buf) {
			ok = 1;
			START_TIME_CONSUMING(1000);
			for( i=0; i<nIter; i++ ) {
				UPDATE_TIME_CONSUMING(1);
				if (!ReadFile(hf, buf, chunk, &bytes_read, NULL) ) {
					END_TIME_CONSUMING;
					AfxMessageBox( "Error reading from file " + CString(path) );
					ok = 0;
					break;
				}
				if(floppy_write( hfloppy, drive, LBA, chunk, buf ) != chunk) {
					END_TIME_CONSUMING;
					AfxMessageBox( "Error writing to floppy." );
					ok = 0;
					break;
				}
				LBA += chunk;
			}
			END_TIME_CONSUMING;
			// free(buf);
			VirtualFree( buf, 0, MEM_RELEASE  ); 
		}

		floppy_final(drive);
	} else {
		AfxMessageBox( "Could not access floppy drive." );
	}
	CloseHandle(hf);
	return(ok);
}

int read_floppy( int drive, char *path )
{
	int i, LBA = 0;
	HANDLE hf;
	char *buf = 0;
	DWORD bytes_read;
	HANDLE hfloppy;
	int C0, H0, S0, nSec;
	int chunk, nIter, ok = 0;

	if(exists(path)) DeleteFile( path );
	hf = CreateFile( path, GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS, 0, NULL );	
	if( hf == INVALID_HANDLE_VALUE ) {
		AfxMessageBox( "Cannot create file " + CString(path) );
		return(0);
	}
	hfloppy = floppy_init( drive, TRUE );
	if(hfloppy) {
		floppy_get_geometry( drive, &C0, &H0, &S0, &nSec );
		chunk = S0 * nSec;
		nIter = C0 * H0;
		// buf = (char *)malloc( chunk );
		buf = (char *)VirtualAlloc( 
				NULL, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
		if(buf) {
			ok = 1;
			START_TIME_CONSUMING(1000);
			for( i=0; i<nIter; i++ ) {
				UPDATE_TIME_CONSUMING(1);
				if(floppy_read( hfloppy, drive, LBA, chunk, buf ) != chunk) {
					END_TIME_CONSUMING;
					AfxMessageBox( "Error reading from floppy." );
					ok = 0;
					break;
				}
				if (!WriteFile(hf, buf, chunk, &bytes_read, NULL) ) {
					END_TIME_CONSUMING;
					AfxMessageBox( "Error writing to file " + CString(path) );
					ok = 0;
					break;
				}
				LBA += chunk;
			}
			END_TIME_CONSUMING;
			// free(buf);
			VirtualFree( buf, 0, MEM_RELEASE  ); 
		}

		floppy_final(drive);
	} else {
		AfxMessageBox( "Could not access floppy drive." );
	}
	CloseHandle(hf);
	if(!ok) {
		if(exists(path)) DeleteFile( path );
	}
	return(ok);
}

static void ReleaseFloppyForWrite(int drive,void* context);

void CMainFrame::OnFileNewHfsPlus() {FormatHfsPlusImage(this,GetBrowserSession());}

void CMainFrame::OnFileNewvolume() 
{
	char volpath[MAX_PATH];
	char vname[MAX_PATH];
	CHFVExplorerApp	*pApp;
	hfv_size_t size; int init;

	pApp = (CHFVExplorerApp *)AfxGetApp();

	*volpath = 0;
	*vname = 0;
	int floppyDrive=-1;
 if(!ask_new_volume(volpath,vname,&size,&init,&floppyDrive))return;
 if(floppyDrive>=0){
  std::vector<BYTE> image;std::string error;
  pc_to_mac_charset((unsigned char*)vname);
  if(!PrepareFloppyHfs(vname,image,error)){AfxMessageBox(error.c_str());return;}
  WriteFloppyImage(this,floppyDrive,image,"Format a new HFS volume",ReleaseFloppyForWrite,GetBrowserSession().Document());
  return;
 }

 auto wide=VolumeCoord::Wide(volpath);auto formatted=ShowStorageJob(CommandStorage::Guard(CommandStorage::ImageWrite(wide),[=](Storage::Job&){LegacyEngine::Scope engine;return HFSIFACE_format(const_cast<char*>(volpath),const_cast<char*>(vname),size,init)==0?Storage::Result{}:Storage::Result{1,ERROR_WRITE_FAULT,"Could not format the selected image"};},{VolumeCoord::ImageKey(wide),VolumeCoord::PathKey(wide)}),"Format HFS image",false,false,false,this);
 if(formatted.code){AfxMessageBox(formatted.message.c_str());return;}

    GetBrowserSession().Document()->OnOpenDocument(volpath);
}

hfv_size_t get_file_size( const char *volpath )
{
	hfv_size_t l = 0;
	CFile file;

	if(do_open_file( &file, volpath )) {
		l = file.GetLength();
		silent_close( &file );
	}
	return(l);
}

int ask_vol_to_floppy( char *volpath, int *pdrive, int to_floppy )
{
	CAskVolumeToFloppy dlg;
	int ret;

	dlg.m_volname = CString(volpath);
	dlg.m_drive = *pdrive;
	dlg.to_floppy = to_floppy;
	if(to_floppy) {
		dlg.m_caption = "Write a volume file to floppy";
	} else {
		dlg.m_caption = "Create a volume file from floppy";
	}
	ret = (dlg.DoModal() == IDOK);
	if(ret) {
		lstrcpy( volpath, dlg.m_volname.GetBuffer(_MAX_PATH) );
		int len = strlen(volpath);
		if( len > 4 && volpath[len-4] != '.' ) {
			strcat( volpath, ".DSK" );
		}
		*pdrive = dlg.m_drive;

		if(*pdrive < 0) {
			AfxMessageBox( "No target drive was selected." );
			ret = 0;
		} else if(!is_any_floppy_present( *pdrive )) {
			AfxMessageBox( "There is no floppy present in the drive. Please insert one and try again." );
			ret = 0;
		} else {
			if(to_floppy) {
				if(!exists(volpath)) {
					AfxMessageBox( "Cannot open the file " + dlg.m_volname + "." );
					ret = 0;
				} else {
					int C0, H0, S0, nSec, sz; hfv_size_t fsz;
					floppy_get_geometry( *pdrive, &C0, &H0, &S0, &nSec );
					sz = C0 * H0 * S0 * nSec;
					fsz = get_file_size(volpath);
					if(fsz > sz) {
						CString cs;
						cs.Format( "The file is larger (%I64u bytes) than the disk size (%d bytes). The file will be truncated, with unpredictable results. Go on anyway?", fsz, sz );
						if(AfxMessageBox( cs, MB_YESNO ) != IDYES) 
						{
							ret = 0;
						}
					}
					if(ret) {
						if(AfxMessageBox( 
							"Ready to format. All old data from the floppy will be destroyed. Go on?",
							MB_YESNO ) != IDYES) 
						{
							ret = 0;
						}
					}
				}
			} else { // from floppy
				if(exists(volpath)) {
					if(AfxMessageBox( 
						"The contents of the old file " + dlg.m_volname + " will be overwritten. Continue?",
						MB_YESNO ) != IDYES) 
					{
						ret = 0;
					}
				}
			}
		}
	}
	return(ret);
}

void CMainFrame::go_home_dir(void)
{
	CHFVExplorerApp	*pApp;
	char dir[_MAX_PATH], *p;

	pApp = (CHFVExplorerApp *)AfxGetApp();
	*dir = 0;
	GetModuleFileName( pApp->m_hInstance, dir, sizeof(dir) );
	if(strlen(dir) < 4) p = dir;
	else {
		p = strrchr( dir, '\\' );
		if(p) *p = 0;
	}
	if(p) _chdir( dir );
}


void CMainFrame::MountPath(char* name)
{
    GetBrowserSession().Document()->OnOpenDocument(name);
}

void CMainFrame::OnFileVolumefromfloppy()
{
 CAskVolumeToFloppy dialog(this);dialog.to_floppy=FALSE;dialog.m_caption="Create a volume file from floppy";
 if(dialog.DoModal()!=IDOK)return;
 BackgroundCopies::StartStorage(MakeFloppyImageJob(dialog.m_drive,(LPCSTR)dialog.m_volname),
     this,"Reading floppy image","Floppy image dump",{},{true});
}

void CMainFrame::OnToolsDumpcddebug()
{
 ShowCdImageDialog(this);
}

MediaControlTarget CMainFrame::SelectedMediaControlTarget() const
{
 auto app=(CHFVExplorerApp*)AfxGetApp();if(!app||!GetBrowserSession().Document()||!GetBrowserSession().Tree())return {};
 auto tree=GetBrowserSession().Tree();HTREEITEM item=tree->m_ct->GetSelectedItem();if(!item)return {};
 DWORD_PTR data=tree->m_ct->GetItemData(item);int type=tree->get_lparam_type(data),index=tree->get_lparam_volinx(data);
 if(type==LIST_MAC){int drive=GetBrowserSession().MacBrowser().SelectedCdDrive();if(drive>=0)return {drive,DRIVE_CDROM};}
 if(type==LIST_HFS&&index>=0&&index<GetBrowserSession().Document()->m_hfs_count)return MacMediaControlTarget(GetBrowserSession().Document()->m_volumes[index].m_file_name);
 if(type==LIST_FAT&&index>=0&&index<GetBrowserSession().Document()->m_fat_count){auto& volume=GetBrowserSession().Document()->m_fats[index];return {toupper((unsigned char)volume.m_letter)-'A',volume.m_type};}
 return {};
}
void CMainFrame::OnCdEjectEjectReload(int reload)
{
 const auto target=SelectedMediaControlTarget();
 if(!CanControlMedia(target))return;
 const auto key=VolumeCoord::DeviceKey(target.drive);
 CommandStorage::Reservation held;
 try{
  held=std::make_shared<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{key,VolumeCoord::Mode::Write}});
 }catch(const VolumeCoord::Busy&){return;}

 const auto mask=DWORD(1)<<target.drive;
 MediaDiscovery::Changed(mask);
 std::vector<std::shared_ptr<MacBrowser::Volume>> released;
 for(auto browser:static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers){
  auto& session=browser->GetBrowserSession();
  session.NavigationMediaChanged(mask);
  auto volumes=session.MacBrowser().DetachCd(target.drive);
  released.insert(released.end(),volumes.begin(),volumes.end());
  auto doc=session.Document();
  for(int i=0;i<doc->m_hfs_count;++i)
   if(MacMediaControlTarget(doc->m_volumes[i].m_file_name).drive==target.drive)
    doc->m_volumes[i].close();
 }
 try{
  auto work=MakeMediaControlJob(target,reload!=0,nullptr,[released=std::move(released),target]{
   for(auto& volume:released)volume->ReleaseMedia();
   native_media_suspend(target.drive);
  },std::move(held));
  BackgroundCopies::StartStorage(std::move(work),this,reload?"Reloading media":"Ejecting media",
    reload?"Reload":"Eject");
 }catch(const std::exception& error){
  MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);
 }
}

void CMainFrame::OnCdEjectEject() 
{
	OnCdEjectEjectReload(0);
}

void CMainFrame::OnCdEjectReload() 
{
	OnCdEjectEjectReload(1);
}

void CMainFrame::OnHelp() 
{
	char executable[MAX_PATH]={};
 DWORD count=GetModuleFileNameA(nullptr,executable,MAX_PATH);
 if(!count||count>=MAX_PATH){AfxMessageBox("Could not locate the help folder.");return;}
 char* slash=strrchr(executable,'\\');if(!slash)return;slash[1]=0;
 CString help=executable;help+="HFVExplorer.chm";
 if(GetFileAttributesA(help)==INVALID_FILE_ATTRIBUTES){AfxMessageBox("HFVExplorer.chm was not found. Keep the help file beside HFVExplorer.exe.");return;}
 HINSTANCE result=ShellExecuteA(GetSafeHwnd(),"open",help,nullptr,executable,SW_SHOWNORMAL);
 if((INT_PTR)result<=32)AfxMessageBox("Windows could not open HFVExplorer.chm.");
}

void CMainFrame::SaveWindowPosition(void)
{
	if(!IsIconic()) {
		RECT r;
		CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
		int zoomed = IsZoomed();
		pApp->WriteProfileInt("WindowPosition","Maximized",zoomed);
		if(!zoomed) {
			GetWindowRect(&r);
			pApp->WriteProfileInt("WindowPosition","Dpi",m_dpi);
            pApp->WriteProfileInt("WindowPosition","x",r.left);
			pApp->WriteProfileInt("WindowPosition","y",r.top);
			pApp->WriteProfileInt("WindowPosition","cx",r.right - r.left);
			pApp->WriteProfileInt("WindowPosition","cy",r.bottom - r.top);
		}
	}
}

void CMainFrame::ActivateFrame(int show){
 if(m_restoreMaximized && show!=SW_HIDE && show!=SW_SHOWMINIMIZED && show!=SW_MINIMIZE && show!=SW_SHOWMINNOACTIVE)show=SW_SHOWMAXIMIZED;
 m_restoreMaximized=false;
 CFrameWnd::ActivateFrame(show);
}
void CMainFrame::LoadWindowPosition(void)
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	int x = pApp->GetProfileInt("WindowPosition","x",15);
	int y = pApp->GetProfileInt("WindowPosition","y",15);
	int cx = pApp->GetProfileInt("WindowPosition","cx",600);
	int cy = pApp->GetProfileInt("WindowPosition","cy",500);
	int zoomed = pApp->GetProfileInt("WindowPosition","Maximized",0);

 m_restoreMaximized=zoomed!=0;
 // Position now, show only after document initialization/registration.
 const int saved = pApp->GetProfileInt("WindowPosition","Dpi",NativeDpi::Base);
 const UINT from = saved > 0 ? UINT(saved) : NativeDpi::Base;
 // Move first so DPI can follow the target monitor before scaling the saved size.
 SetWindowPos(nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
 SetWindowPos(nullptr,0,0,NativeDpi::Scale(cx,m_dpi,from),
    NativeDpi::Scale(cy,m_dpi,from),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}

void CMainFrame::OnEditCopy() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_copy();
}

void CMainFrame::OnEditCut() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_cut();
}

void CMainFrame::OnScanRemovable(){GetBrowserSession().MacBrowser().ScanRemovable();}
void CMainFrame::OnPasteHardLink(){GetBrowserSession().MacBrowser().PasteHardLink();}
void CMainFrame::OnUpdatePasteHardLink(CCmdUI* ui){ui->Enable(!StorageUi::busy()&&GetBrowserSession().MacBrowser().CanPasteHardLink());}
void CMainFrame::OnEditPaste() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_paste();
}

void CMainFrame::OnEditCopyAppend() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_copy_append();
}

void CMainFrame::OnEditCutAppend() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_cut_append();
}

void CMainFrame::OnEditSelectAll() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	GetBrowserSession().List()->edit_select_all();
}

BOOL CMainFrame::OnQueryOpen() 
{
	// AfxMessageBox( "OnQueryOpen" );
	
	return CFrameWnd::OnQueryOpen();
}

void CMainFrame::OnClose() 
{
 if(BackgroundCopies::Active()&&static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers.size()<=1){
  BackgroundCopies::CancelAll();
  SetTimer(9201,100,nullptr);if(GetBrowserSession().Status())GetBrowserSession().Status()->SetPaneText(0,"Closing after copy cleanup finishes...");return;
 }

	BOOL do_close = TRUE;
	if(!m_launchedDocuments.empty()) {
		int answer = AfxMessageBox( 
			"There might be some launched documents open. If you terminate HFVExplorer now, any possible changes will not be written back to the HFS volumes, and the temporary files are not deleted.\r\n\r\nClose HFVExplorer anyway?",
			MB_YESNO | MB_ICONQUESTION
		);
		if(answer == IDNO) do_close = FALSE;
	}
	
 if(do_close){
  // CFrameWnd::OnClose shuts down every document when this is m_pMainWnd.
  // Each browser is a separate document: close only this one instead.
  auto document=GetBrowserSession().Document();
  if(document&&document->CanCloseFrame(this))document->OnCloseDocument();
 }
}

void CMainFrame::OnFileEject() 
{
	OnCdEjectEjectReload(0);
}

void CMainFrame::OnFileReload() 
{
	OnCdEjectEjectReload(1);
}

// move to .h
extern void get_execute_error_string( HINSTANCE hi, CString &s );

void CMainFrame::OnToolsBurn()
{
 CString initial;
 CHFVExplorerApp* app=(CHFVExplorerApp*)AfxGetApp();
 if(GetBrowserSession().Tree()&&GetBrowserSession().Document()){
  HTREEITEM item=GetBrowserSession().Tree()->m_ct->GetSelectedItem();
  if(item){DWORD_PTR data=GetBrowserSession().Tree()->m_ct->GetItemData(item);
   if(GetBrowserSession().Tree()->get_lparam_type(data)==LIST_HFS){
    int index=GetBrowserSession().Tree()->get_lparam_volinx(data);
    if(index>=0&&index<GetBrowserSession().Document()->m_hfs_count){auto& volume=GetBrowserSession().Document()->m_volumes[index];
     if(!volume.m_is_floppy&&!volume.m_is_cd&&!volume.m_is_hd&&!volume.m_is_removable)initial=volume.m_file_name;
    }
   }
  }
 }
 ShowCdBurnDialog(this,initial);
}

void CMainFrame::OnUpdatePhysicalMedia(CCmdUI* ui) {ui->Enable(!StorageUi::busy()&&!StorageUi::state().sessionOpen&&CommandAllowed(ui->m_nID));}

static void ReleaseFloppyForWrite(int drive,void* context)
{
 CHFVExplorerDoc* document=(CHFVExplorerDoc*)context;
 for(int i=0;i<document->m_hfs_count;++i)
  if(physical_floppy_path(document->m_volumes[i].m_file_name)==drive) document->m_volumes[i].close();
}
void CMainFrame::OnFileVolumetofloppy()
{
 CHFVExplorerApp* app=(CHFVExplorerApp*)AfxGetApp();
 CAskVolumeToFloppy dialog(this);dialog.to_floppy=TRUE;dialog.m_caption="Write a volume file to floppy";
 if(dialog.DoModal()!=IDOK)return;
 std::vector<BYTE> image;std::string error;
 auto loaded=ShowStorageWaitJob(CommandStorage::Guard({{VolumeCoord::ImageKey(std::string((LPCSTR)dialog.m_volname)),VolumeCoord::Mode::Read}},[&](Storage::Job&){return LoadFloppyHfs(dialog.m_volname,image,error)?Storage::Result{}:Storage::Result{1,ERROR_READ_FAULT,error};}));if(loaded.code){AfxMessageBox(loaded.message.c_str());return;}
 WriteFloppyImage(this,dialog.m_drive,image,dialog.m_volname,ReleaseFloppyForWrite,GetBrowserSession().Document());
}

void CMainFrame::OnToolsFind()
{
 auto& session=GetBrowserSession();
 if(!session.ListingAvailable())return;
 std::optional<FindScope> scope;
 try{scope=session.CaptureFindScope();}
 catch(const std::exception&){}
 if(!scope){AfxMessageBox("Select a folder or volume to search first.");return;}
 FindHit selected;
 if(ShowFindFilesDialog(this,std::move(*scope),selected)&&!session.RevealFindHit(selected))
  session.RejectNavigation({},"The search volume is no longer attached.");
}

BOOL CMainFrame::PreTranslateMessage(MSG* message){
 if(message->message==WM_KEYDOWN&&message->wParam==VK_ESCAPE&&m_browserSession){auto list=GetBrowserSession().List();if(list&&list->IsDragging()){list->SendMessage(WM_CANCELMODE);return TRUE;}}
 return CFrameWnd::PreTranslateMessage(message);
}
BOOL CMainFrame::OnCommand(WPARAM w,LPARAM l){
 switch(LOWORD(w)){
 case ID_FILE_MAC_IMPORT:GetBrowserSession().MacBrowser().Import();return TRUE;
 case ID_FILE_MAC_IMPORT_RESOURCE:GetBrowserSession().MacBrowser().ImportResource();return TRUE;
 case ID_FILE_MAC_EXPORT_DATA:GetBrowserSession().MacBrowser().Export(0);return TRUE;
 case ID_FILE_MAC_EXPORT_RESOURCE:GetBrowserSession().MacBrowser().Export(1);return TRUE;
 }
 return CFrameWnd::OnCommand(w,l);
}
void CMainFrame::OnUpdateMacTransfer(CCmdUI* ui){
 auto& browser=GetBrowserSession().MacBrowser();bool enabled=false;
 if(!StorageUi::busy()){
  if(ui->m_nID==ID_FILE_MAC_IMPORT)enabled=browser.Writable();
  else if(ui->m_nID==ID_FILE_MAC_IMPORT_RESOURCE)enabled=browser.CanExport()&&browser.SelectionWritable();
  else enabled=browser.CanExport();
 }
 ui->Enable(enabled);
}
void CMainFrame::OnUpdateMacEdit(CCmdUI* ui){
 auto& browser=GetBrowserSession().MacBrowser();auto app=static_cast<CHFVExplorerApp*>(AfxGetApp());
 bool allowed=browser.Active()?(ui->m_nID==ID_EDIT_CUT?browser.SelectionWritable():browser.Writable()):
     GetBrowserSession().List()&&(ui->m_nID==ID_EDIT_CUT?GetBrowserSession().List()->BrowserSelectionAllows(false):GetBrowserSession().List()->BrowserDirectoryWritable());
 ui->Enable(!StorageUi::busy()&&allowed&&(ui->m_nID!=ID_EDIT_PASTE||BrowserTransfers::Any()));
}
