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
#include "HfsPlusBrowser.h"
#include "DeviceWait.h"
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
 ON_UPDATE_COMMAND_UI(ID_FILE_EJECT, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_FILE_RELOAD, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_CD_EJECT_RELOAD, OnUpdatePhysicalMedia)
 ON_UPDATE_COMMAND_UI(ID_CD_EJECT_EJECT, OnUpdatePhysicalMedia)


	//{{AFX_MSG_MAP(CMainFrame)
	ON_WM_CREATE()
	ON_COMMAND(ID_HFV_VIEW_DETAILS, OnViewDetails)
	ON_COMMAND(ID_VIEW_LARGEICONS, OnViewLargeicons)
	ON_COMMAND(ID_HFV_VIEW_LIST, OnViewList)
	ON_COMMAND(ID_VIEW_SMALLICONS, OnViewSmallicons)
	ON_COMMAND(ID_VIEW_OPTIONS, OnViewOptions)
	ON_WM_ACTIVATEAPP()
	ON_WM_TIMER()
	ON_WM_QUERYNEWPALETTE()
	ON_WM_PALETTECHANGED()
	ON_WM_DROPFILES()
	ON_COMMAND(ID_FILE_NEWVOLUME, OnFileNewvolume)
 ON_COMMAND(ID_FILE_NEWHFSPLUS, OnFileNewHfsPlus)
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
	pApp->m_main = this;
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

	if(pApp->get_font()) {
		::SendMessage( 
			m_wndStatusBar.GetSafeHwnd(),
			WM_SETFONT,
			(WPARAM)pApp->get_font(),
			MAKELPARAM(TRUE,0) );
	}

	// TODO: Remove this if you don't want tool tips or a resizeable toolbar
	m_wndToolBar.SetBarStyle(m_wndToolBar.GetBarStyle() |
		CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC);

	// TODO: Delete these three lines if you don't want the toolbar to
	//  be dockable
	m_wndToolBar.EnableDocking(CBRS_ALIGN_ANY);
	EnableDocking(CBRS_ALIGN_ANY);
	DockControlBar(&m_wndToolBar);

	pApp->m_pStatusBar = &m_wndStatusBar;

	pApp->m_wndSplitter = &m_wndSplitter;
	pApp->m_wndSplitter2 = &m_wndSplitter2;

	HFSIFACE_init( GetSafeHwnd() );

	LoadWindowPosition();

	return 0;
}

BOOL CMainFrame::PreCreateWindow(CREATESTRUCT& cs)
{
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

// #define TWOVIEWS

#ifdef TWOVIEWS
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

	if (!m_wndSplitter.CreateView(0, PANE_LIST,
		RUNTIME_CLASS(CHFVExplorerListView), CSize(300, 0), pContext))
	{
		TRACE0("Failed to create second pane\n");
		return FALSE;
	}

	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	pApp->m_list = (CHFVExplorerListView *)m_wndSplitter.GetPane( 0, PANE_LIST );
	pApp->m_list->Initialize();
	pApp->m_list->set_style( LVS_ICON );
	pApp->m_list->FillList();

	pApp->m_tree = (HFVExplorerTreeView *)m_wndSplitter.GetPane( 0, PANE_TREE );
	pApp->m_tree->Initialize();

	m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );

	return TRUE;
	// return CFrameWnd::OnCreateClient(lpcs, pContext);
}
#else
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

	int cyText = max(lpcs->cy - 70, 20);    // height of text pane
	if (!m_wndSplitter2.CreateView(0, 0,
		RUNTIME_CLASS(CHFVExplorerListView), CSize(0, cyText), pContext))
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

	pApp->m_tree = (HFVExplorerTreeView *)m_wndSplitter.GetPane( 0, 0 );
	pApp->m_tree->Initialize();

	pApp->m_list = (CHFVExplorerListView *)m_wndSplitter2.GetPane( 0, 0 );
	pApp->m_list->Initialize();
	pApp->m_list->set_style( LVS_ICON );
	pApp->m_list->FillList();

	pApp->m_pre = (CHFVPreviewView *)m_wndSplitter2.GetPane( 1, 0 );
	pApp->m_pre->Initialize();

	m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );

	return TRUE;
	// return CFrameWnd::OnCreateClient(lpcs, pContext);
}
#endif

void CMainFrame::OnViewDetails() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (pApp->m_mac_window_mode == TRUE);
	pApp->m_mac_window_mode = FALSE;
	pApp->m_list->set_style( LVS_REPORT );
	if(changed_mode) pApp->m_tree->mode_changed();
}

void CMainFrame::OnViewLargeicons() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (pApp->m_mac_window_mode == TRUE);
	pApp->m_mac_window_mode = FALSE;
	pApp->m_list->set_style( LVS_ICON );
	if(changed_mode) pApp->m_tree->mode_changed();
}

void CMainFrame::OnViewList() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (pApp->m_mac_window_mode == TRUE);
	pApp->m_mac_window_mode = FALSE;
	pApp->m_list->set_style( LVS_LIST );
	if(changed_mode) pApp->m_tree->mode_changed();
}

void CMainFrame::OnViewSmallicons() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (pApp->m_mac_window_mode == TRUE);
	pApp->m_mac_window_mode = FALSE;
	pApp->m_list->set_style( LVS_SMALLICON );
	if(changed_mode) pApp->m_tree->mode_changed();
}

void CMainFrame::OnViewMacWindow() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int changed_mode = (pApp->m_mac_window_mode == FALSE);
	pApp->m_mac_window_mode = TRUE;
	pApp->m_list->set_style( LVS_ICON );
	if(changed_mode) pApp->m_tree->mode_changed();
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

		pApp->m_doc->update_invisible_attributes();

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

void CMainFrame::OnActivateApp(BOOL bActive, DWORD hTask)
{
	CFrameWnd::OnActivateApp(bActive, hTask);
	
	if(bActive) {
		// check HFV files for changes
		CHFVExplorerApp	*pApp;
		pApp = (CHFVExplorerApp *)AfxGetApp();
		pApp->m_doc->check_hfv_file_times( 0 );
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

static HANDLE get_top_window_phandle( void )
{
	DWORD pid;
	HANDLE hProcess;
	HWND hwnd;

	hwnd = ::GetForegroundWindow();
	GetWindowThreadProcessId( hwnd, &pid );
	// Must open the process with minimal access rights
	hProcess = OpenProcess(STANDARD_RIGHTS_REQUIRED |PROCESS_QUERY_INFORMATION, FALSE, pid);
	return( hProcess );
}

void CMainFrame::OnTimer(UINT_PTR nIDEvent)
{
 if(HfvDeviceWait::busy()||StorageUi::state().sessionOpen)return;
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	int i, count;
	wait_item_struct *w;
	static BOOL inhere = FALSE;

	if(inhere) return;

	inhere = TRUE;

	count = m_wait_items.GetSize();
	for(i=count-1; i>=0; i--) {
		w = (wait_item_struct *)m_wait_items.ElementAt(i);
		if(w->m_waiting_emulator_process) {
			DWORD exitcode;
			// Windows NT: The handle must have PROCESS_QUERY_INFORMATION access
			if(GetExitCodeProcess(w->m_waiting_emulator_process,&exitcode)) {
				if(exitcode != STILL_ACTIVE) {

					// Has it died very soon? Maybe it launched some other app,
					// or activated a previous instance.

					BOOL going_away = TRUE;

					if(GetTickCount() < w->m_start_watch_emulator) {
						HANDLE hProcess = get_top_window_phandle();
						if(hProcess) {
							w->m_waiting_emulator_process = hProcess;
							// CloseHandle(hProcess);
							going_away = FALSE;
						}
						w->m_start_watch_emulator = 0;
					}

					if(going_away && can_have_exclusive_access(w->m_waiting_doc_win_name)) {
						m_wait_items.RemoveAt(i);
						w->m_waiting_emulator_process = 0;
						if(m_was_zoomed) {
							ShowWindow( SW_RESTORE );
							ShowWindow( SW_SHOWMAXIMIZED );
						} else {
							ShowWindow( SW_RESTORE );
						}
						SetForegroundWindow();
						UpdateWindow();
						m_was_zoomed = 0;

						CFileStatus rStatus;
						BOOL do_update = FALSE;

						if(CFile::GetStatus(w->m_waiting_doc_win_name,rStatus)) {
							if( w->m_waiting_emu_mtime != rStatus.m_mtime ||
									w->m_waiting_emu_size != rStatus.m_size)
							{
								int answer = AfxMessageBox( 
									"The file " + CString(w->m_waiting_doc_win_name) + 
									" was changed. Do yo want to update the original file?",
									MB_YESNO | MB_ICONQUESTION
								);
								if(answer == IDYES) do_update = TRUE;
							}
						}
						pApp->m_list->update_callback( 
							w->m_waiting_doc_win_name,
							w->m_waiting_srcvolpath,
							w->m_waiting_srcname,
							do_update
						);
						delete w;
					}
				}
			}
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
		if(pApp->m_doc && pApp->m_list && pApp->m_tree) {
			// if(m_timer) KillTimer(m_timer);
			// m_timer = 0;
			m_volcheck_enabled = FALSE;
			pApp->m_doc->check_hfv_file_times( 0 );
			m_volcheck_enabled = TRUE;
			// if(!m_timer) m_timer = SetTimer( MYTIMERID, VOLCHECK_TIMER_DELAY, 0 );
		}
	}

	inhere = FALSE;

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

void CMainFrame::WaitEmulator2( 
	HANDLE process,
	LPCSTR doc_win_name,
	LPCSTR srcvolpath,
	LPCSTR srcname
)
{
	CFileStatus rStatus;
	wait_item_struct *w;

	w = new wait_item_struct;

	if(process == 0) {
		process = get_top_window_phandle();
	}

	w->m_waiting_emulator_process = process;
	strcpy( w->m_waiting_doc_win_name, doc_win_name );
	strcpy( w->m_waiting_srcvolpath, srcvolpath );
	strcpy( w->m_waiting_srcname, srcname );

	if(CFile::GetStatus(doc_win_name,rStatus)) {
		w->m_waiting_emu_mtime = rStatus.m_mtime;
		w->m_waiting_emu_size = rStatus.m_size;
	}
	w->m_start_watch_emulator = GetTickCount() + 2500ul;
	// get_process_list( w->m_process_list );

	m_wait_items.Add( (DWORD_PTR)w );

	m_was_zoomed = IsZoomed();
	// ShowWindow( SW_MINIMIZE );
}

BOOL CMainFrame::DestroyWindow() 
{
	SaveWindowPosition();
	if(m_timer) KillTimer(m_timer);
	m_timer = 0;
	m_volcheck_enabled = FALSE;
	
	return CFrameWnd::DestroyWindow();
}

void CMainFrame::OnPaletteChanged(CWnd* pFocusWnd) 
{
	CFrameWnd::OnPaletteChanged(pFocusWnd);
	
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->_OnPaletteChanged(pFocusWnd);
}

BOOL CMainFrame::OnQueryNewPalette() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	return(pApp->m_list->_OnQueryNewPalette());
	// return CFrameWnd::OnQueryNewPalette();
}

void CMainFrame::OnDropFiles(HDROP hDropInfo) 
{
	int handled = 0;
	POINT p;
	char name[MAX_PATH];
	int i, count;
	CHFVExplorerApp	*pApp;
	CString currdir = "";

	pApp = (CHFVExplorerApp *)AfxGetApp();

	count = DragQueryFile( hDropInfo, 0xFFFFFFFF, 0, 0 );
	for(i=0; i<count; i++ ){
		if(DragQueryFile( hDropInfo, i, name, MAX_PATH-1 ) > 0) {
		  DragQueryPoint( hDropInfo, &p );
			if(is_extension( name, ".HF*" ) || is_extension( name, ".DSK" )) {
				int save = pApp->m_doc->m_hfs_count;
				pApp->m_doc->OnOpenDocument(name);
				if(save < pApp->m_doc->m_hfs_count) {
					currdir = pApp->m_doc->m_volumes[pApp->m_doc->m_hfs_count-1].m_volume_name + ":";
				}
			}
		}
	}
	if(currdir != "") pApp->m_doc->my_chdir( currdir, 1 );

	DragFinish(hDropInfo);
	handled = 1;

	if(!handled) {
		CFrameWnd::OnDropFiles(hDropInfo);
	}
}

void CMainFrame::OnChildRegistration(WPARAM wParam, LPARAM lParam) 
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->add_child( (HWND)lParam );
}

LRESULT CMainFrame::WindowProc(UINT message, WPARAM wParam, LPARAM lParam) 
{
 if(message==WM_COMMAND&&!StorageUi::busy()){
  StorageAction storageAction;
  return CFrameWnd::WindowProc(message,wParam,lParam);
 }
 if(message==WM_DEVICECHANGE&&StorageUi::state().sessionOpen){
  if((wParam==DBT_DEVICEARRIVAL||wParam==DBT_DEVICEREMOVECOMPLETE)&&lParam&&
     reinterpret_cast<DEV_BROADCAST_HDR*>(lParam)->dbch_devicetype==DBT_DEVTYP_VOLUME)m_deviceChangeDeferred=true;
  return TRUE;
 }
 if(HfvDeviceWait::busy()) {
  if(message==WM_DEVICECHANGE) {
   if((wParam==DBT_DEVICEARRIVAL||wParam==DBT_DEVICEREMOVECOMPLETE)&&lParam&&
      reinterpret_cast<DEV_BROADCAST_HDR*>(lParam)->dbch_devicetype==DBT_DEVTYP_VOLUME)m_deviceChangeDeferred=true;
   return TRUE;
  }
  if(message==WM_COMMAND || message==WM_CLOSE)return 0;
 }
 if(message==WM_APP+91 && m_deviceChangeDeferred) {
  m_deviceChangeDeferred=false;PostMessage(WM_COMMAND,ID_VIEW_REFRESH);return 0;
 }
	if(message == our_child_registration) {
		OnChildRegistration(wParam,lParam);
	} else if(message == WM_DEVICECHANGE) {
		if( wParam == DBT_DEVICEREMOVECOMPLETE ) {
			DEV_BROADCAST_HDR *p;
			p = (DEV_BROADCAST_HDR *)lParam;
			// AfxMessageBox( "Device removed" );
			if(p->dbch_devicetype == DBT_DEVTYP_VOLUME) {
				// AfxMessageBox( "DBT_DEVTYP_VOLUME" );
				CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
				pApp->m_doc->CheckNewMedia( FALSE, TRUE, TRUE );
			}
		} else if( wParam == DBT_DEVICEARRIVAL ) {
			DEV_BROADCAST_HDR *p;
			p = (DEV_BROADCAST_HDR *)lParam;
			// AfxMessageBox( "Device arrived" );
			if(p->dbch_devicetype == DBT_DEVTYP_VOLUME) {
				// AfxMessageBox( "DBT_DEVTYP_VOLUME" );
				CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
				pApp->m_doc->CheckNewMedia( FALSE, TRUE, TRUE );
			}
		}
	}
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
		writeback_flush_all();
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
		writeback_flush_all();
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

void CMainFrame::OnFileNewHfsPlus() {FormatHfsPlusImage(this);}

void CMainFrame::OnFileNewvolume() 
{
	char volpath[MAX_PATH];
	char vname[MAX_PATH];
	CHFVExplorerApp	*pApp;
	CString currdir = "";
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
  WriteFloppyImage(this,floppyDrive,image,"Format a new HFS volume",ReleaseFloppyForWrite,pApp->m_doc);
  SendMessage(WM_COMMAND,ID_VIEW_REFRESH);return;
 }

	if(0 != HFSIFACE_format( volpath, vname, size, init )) {
		return;
	}

	int save = pApp->m_doc->m_hfs_count;
	pApp->m_doc->OnOpenDocument(volpath);
	if(save < pApp->m_doc->m_hfs_count) {
		currdir = pApp->m_doc->m_volumes[pApp->m_doc->m_hfs_count-1].m_volume_name + ":";
	}

	if(currdir != "") pApp->m_doc->my_chdir( currdir, 1 );
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


void CMainFrame::MountPath(char *name) 
{
	CHFVExplorerApp	*pApp;
	CString currdir = "";

	pApp = (CHFVExplorerApp *)AfxGetApp();

	int save = pApp->m_doc->m_hfs_count;
	pApp->m_doc->OnOpenDocument(name);
	if(save < pApp->m_doc->m_hfs_count) {
		currdir = pApp->m_doc->m_volumes[pApp->m_doc->m_hfs_count-1].m_volume_name + ":";
	}
	if(currdir != "") pApp->m_doc->my_chdir( currdir, 1 );
}

void CMainFrame::OnFileVolumefromfloppy()
{
 CAskVolumeToFloppy dialog(this);dialog.to_floppy=FALSE;dialog.m_caption="Create a volume file from floppy";
 if(dialog.DoModal()!=IDOK)return;
 auto result=ShowStorageJob(MakeFloppyImageJob(dialog.m_drive,(LPCSTR)dialog.m_volname),"Reading floppy image",false,true,true);
 if(result.code&&result.error!=ERROR_CANCELLED)AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
}

void CMainFrame::OnToolsDumpcddebug()
{
 ShowCdImageDialog(this);
}

#if 0
void CMainFrame::OnCdEjectReload() 
{
	char rootdir[20], letter;
	int type;

	for( letter = 'C'; letter <= 'Z'; letter++ ) {
		wsprintf( rootdir, "%c:\\", letter );
		type = GetDriveType( rootdir );
		if(type == DRIVE_CDROM /*|| type == DRIVE_REMOVABLE */) {
			eject_media( letter, 1 );
		}
	}
}

void CMainFrame::OnCdEjectEject() 
{
	char rootdir[20], letter;
	int type;


	for( letter = 'C'; letter <= 'Z'; letter++ ) {
		wsprintf( rootdir, "%c:\\", letter );
		type = GetDriveType( rootdir );
		if(type == DRIVE_CDROM /*|| type == DRIVE_REMOVABLE */) {
			eject_media( letter, 0 );
		}
	}
}
#endif

MediaControlTarget CMainFrame::SelectedMediaControlTarget() const
{
 auto app=(CHFVExplorerApp*)AfxGetApp();if(!app||!app->m_doc||!app->m_tree)return {};
 auto tree=app->m_tree;HTREEITEM item=tree->m_ct->GetSelectedItem();if(!item)return {};
 DWORD_PTR data=tree->m_ct->GetItemData(item);int type=tree->get_lparam_type(data),index=tree->get_lparam_volinx(data);
 if(type==LIST_HFS&&index>=0&&index<app->m_doc->m_hfs_count)return MacMediaControlTarget(app->m_doc->m_volumes[index].m_file_name);
 if(type==LIST_FAT&&index>=0&&index<app->m_doc->m_fat_count){auto& volume=app->m_doc->m_fats[index];return {toupper((unsigned char)volume.m_letter)-'A',volume.m_type};}
 return {};
}
void CMainFrame::OnCdEjectEjectReload(int reload)
{
 auto target=SelectedMediaControlTarget();if(!CanControlMedia(target))return;
 auto app=(CHFVExplorerApp*)AfxGetApp();auto document=app->m_doc;
 auto result=ShowStorageWaitJob(MakeMediaControlJob(target,reload!=0,nullptr,[=]{
  HFSIFACE_get_dir_flush();
  for(int i=0;i<document->m_hfs_count;++i){auto mounted=MacMediaControlTarget(document->m_volumes[i].m_file_name);if(mounted.drive==target.drive)document->m_volumes[i].close();}
  native_media_suspend(target.drive);
 }));
 if(result.code)AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
 m_deviceChangeDeferred=false;PostMessage(WM_COMMAND,ID_VIEW_REFRESH);
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
			pApp->WriteProfileInt("WindowPosition","x",r.left);
			pApp->WriteProfileInt("WindowPosition","y",r.top);
			pApp->WriteProfileInt("WindowPosition","cx",r.right - r.left);
			pApp->WriteProfileInt("WindowPosition","cy",r.bottom - r.top);
		}
	}
}

void CMainFrame::LoadWindowPosition(void)
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	int x = pApp->GetProfileInt("WindowPosition","x",15);
	int y = pApp->GetProfileInt("WindowPosition","y",15);
	int cx = pApp->GetProfileInt("WindowPosition","cx",600);
	int cy = pApp->GetProfileInt("WindowPosition","cy",500);
	int zoomed = pApp->GetProfileInt("WindowPosition","Maximized",0);

	if(zoomed) {
		SetWindowPos( NULL, x, y, cx, cy, SWP_NOZORDER );
		ShowWindow( SW_SHOWMAXIMIZED );
	}	else {
		SetWindowPos( NULL, x, y, cx, cy, SWP_NOZORDER );
	}
}

void CMainFrame::OnEditCopy() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_copy();
}

void CMainFrame::OnEditCut() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_cut();
}

void CMainFrame::OnEditPaste() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_paste();
}

void CMainFrame::OnEditCopyAppend() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_copy_append();
}

void CMainFrame::OnEditCutAppend() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_cut_append();
}

void CMainFrame::OnEditSelectAll() 
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	pApp->m_list->edit_select_all();
}

BOOL CMainFrame::OnQueryOpen() 
{
	// AfxMessageBox( "OnQueryOpen" );
	
	return CFrameWnd::OnQueryOpen();
}

void CMainFrame::OnClose() 
{
	BOOL do_close = TRUE;
	int count = m_wait_items.GetSize();
	if(count != 0) {
		int answer = AfxMessageBox( 
			"There might be some launched documents open. If you terminate HFVExplorer now, any possible changes will not be written back to the HFS volumes, and the temporary files are not deleted.\r\n\r\nClose HFVExplorer anyway?",
			MB_YESNO | MB_ICONQUESTION
		);
		if(answer == IDNO) do_close = FALSE;
	}
	
	if(do_close) CFrameWnd::OnClose();
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
 if(app->m_tree&&app->m_doc){
  HTREEITEM item=app->m_tree->m_ct->GetSelectedItem();
  if(item){DWORD_PTR data=app->m_tree->m_ct->GetItemData(item);
   if(app->m_tree->get_lparam_type(data)==LIST_HFS){
    int index=app->m_tree->get_lparam_volinx(data);
    if(index>=0&&index<app->m_doc->m_hfs_count){auto& volume=app->m_doc->m_volumes[index];
     if(!volume.m_is_floppy&&!volume.m_is_cd&&!volume.m_is_hd&&!volume.m_is_removable)initial=volume.m_file_name;
    }
   }
  }
 }
 ShowCdBurnDialog(this,initial);
}

void CMainFrame::OnUpdatePhysicalMedia(CCmdUI* ui) {ui->Enable(!StorageUi::busy()&&!StorageUi::state().sessionOpen&&CanControlMedia(SelectedMediaControlTarget()));}

static void ReleaseFloppyForWrite(int drive,void* context)
{
 CHFVExplorerDoc* document=(CHFVExplorerDoc*)context;
 HFSIFACE_get_dir_flush();
 for(int i=0;i<document->m_hfs_count;++i)
  if(document->m_volumes[i].m_file.m_hfloppy && document->m_volumes[i].m_file.m_drive==drive) document->m_volumes[i].close();
}
void CMainFrame::OnFileVolumetofloppy()
{
 CHFVExplorerApp* app=(CHFVExplorerApp*)AfxGetApp();
 CAskVolumeToFloppy dialog(this);dialog.to_floppy=TRUE;dialog.m_caption="Write a volume file to floppy";
 if(dialog.DoModal()!=IDOK)return;
 std::vector<BYTE> image;std::string error;
 if(!LoadFloppyHfs(dialog.m_volname,image,error)){AfxMessageBox(error.c_str());return;}
 WriteFloppyImage(this,dialog.m_drive,image,dialog.m_volname,ReleaseFloppyForWrite,app->m_doc);
 SendMessage(WM_COMMAND,ID_VIEW_REFRESH);
}

void CMainFrame::OnToolsFind()
{
 CHFVExplorerApp* app=(CHFVExplorerApp*)AfxGetApp();
 if(!app->m_doc||!app->m_tree)return;
 HTREEITEM item=app->m_tree->m_ct->GetSelectedItem();
 if(!item){AfxMessageBox("Select a folder or volume to search first.");return;}
 DWORD_PTR data=app->m_tree->m_ct->GetItemData(item);int type=app->m_tree->get_lparam_type(data),index=app->m_tree->get_lparam_volinx(data);
 int mac=0;CString path=app->m_doc->get_directory(item,&mac);FindLocation location;
 location.mac=mac!=0;location.directory=(LPCSTR)path;location.volumeIndex=index;
 if(path.IsEmpty()){AfxMessageBox("Select a folder or volume to search first.");return;}
 if(type==LIST_HFS&&index>=0&&index<app->m_doc->m_hfs_count){location.volume=(LPCSTR)app->m_doc->m_volumes[index].m_file_name;location.root=(LPCSTR)(app->m_doc->m_volumes[index].m_volume_name+":");}
 else if(type==LIST_FAT&&index>=0&&index<app->m_doc->m_fat_count){location.root=std::string(1,app->m_doc->m_fats[index].m_letter)+":\\";}
 else {AfxMessageBox("Select a folder or volume to search first.");return;}
 FindHit selected;if(!ShowFindFilesDialog(this,location,selected))return;
 app->m_doc->my_chdir(selected.parent.c_str(),location.mac,location.mac?index:-1);
 if(app->m_list){auto& list=app->m_list->GetListCtrl();
  for(int row=0;row<list.GetItemCount();++row)if(list.GetItemText(row,0).CompareNoCase(selected.name.c_str())==0){
   list.SetItemState(-1,0,LVIS_SELECTED|LVIS_FOCUSED);list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);list.EnsureVisible(row,FALSE);app->m_list->SetFocus();return;
  }
 }
 AfxMessageBox("The folder was opened, but the item is not visible. It may have changed or be hidden by the current view settings.");
}
