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
#include "WindowsCompatibility.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "HFVExplorer.h"
#include "BrowserTransfers.h"

#include "MainFrm.h"
#include "HFVExplorerDoc.h"
#include "HFVExplorerView.h"
#include "HFVCommandLineInfo.h"
#include "floppy.h"
#include "hfs\libhfs\hfs.h"
#include "hfs\interface.h"
#include "special.h"
#include "tmap.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerApp

BEGIN_MESSAGE_MAP(CHFVExplorerApp, CWinApp)
	//{{AFX_MSG_MAP(CHFVExplorerApp)
	ON_COMMAND(ID_APP_ABOUT, OnAppAbout)
	ON_COMMAND(ID_FILE_OPEN, OnFileOpen)
	//}}AFX_MSG_MAP
	// Standard file based document commands
 ON_COMMAND(ID_FILE_NEW, OnNewBrowser)
 ON_COMMAND(ID_TOOLS_NEW_BROWSER, OnNewBrowser)
 ON_UPDATE_COMMAND_UI(ID_TOOLS_NEW_BROWSER, OnUpdateNewBrowser)
 ON_UPDATE_COMMAND_UI(ID_FILE_NEW, OnUpdateNewBrowser)
 ON_COMMAND(ID_APP_EXIT, OnExitBrowsers)
END_MESSAGE_MAP()

	// ON_COMMAND(ID_FILE_OPEN, CWinApp::OnFileOpen)

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerApp construction

CHFVExplorerApp theApp;

// Quick hack from "volume.c"
extern "C" void set_invalid_cd_hack( int onoff );

/* changed by noda */
void CHFVExplorerApp::set_font( char *name, int ps )
{
	int	charset = 0; /* ascii */

	if(m_hfont) {
		DeleteObject( m_hfont );
		m_hfont = 0;
	}
	/* noda */
	if(m_is_font_sjis == TRUE){
		charset = 128;
	}
	m_hfont = CreateFont( -ps, 0, 0, 0, FW_NORMAL, (BYTE)FALSE, 0, 0, charset, 0, 0, 0, 0, name ); /* SHIFT_JIS */
}

CHFVExplorerApp::CHFVExplorerApp()
{
	m_pStatusBar = 0;
 m_main=nullptr;m_doc=nullptr;m_tree=nullptr;m_list=nullptr;m_pre=nullptr;m_wndSplitter=nullptr;m_wndSplitter2=nullptr;
	m_opendir = "";
	is_desktop_application = 1;
	m_parent_hwnd = 0;
	m_startdir_processed = 0;
	m_dosicons = 1;
	m_fat_icon_time_limit = 0;
	m_mountall = TRUE;
	m_startup_floppies = TRUE;
	m_startup_cds = TRUE;
	m_startup_hds = TRUE;
	m_copyout_mode = 5;
	m_copyin_mode = 5;
	m_copyin_appledouble = TRUE;
	m_timer_enabled = TRUE;
	m_version = "";
	// m_hfont = CreateFont( -point_size, 0, 0, 0, FW_NORMAL, (BYTE)FALSE, 0, 0, 0, 0, 0, 0, 0, font_name );
	m_is_font_sjis = FALSE; /* noda */
	set_font( font_name, point_size );
	m_next_icon_index = 0;
	m_show_invisible_mac = 0;
	m_show_invisible_fat = 1;
	m_protect_locked_mac = 0;
	m_protect_locked_fat = 0;

	m_executor_dos_path = _T("");
	m_executor_win32_path = _T("");
	m_vmac_path = _T("");
	m_vmac_startup_folder_name = _T("");
	m_vmac_system_file_path = _T("");
	m_default_emulator = 5;
	m_link_name_style = 2;

	m_launch_method = 0;
	m_use_remover = 1;

	m_shapeshifter_path = _T("");
	m_shapeshifter_startup_folder_name = _T("");
	m_shapeshifter_system_file_path = _T("");
	m_shapeshifter_preferences_file_path = _T("");
	m_shapeshifter_amiga_volume_name = _T("");
	m_shapeshifter_use_remover = 1;

	m_fusion_startup_folder_name = _T("");
	m_fusion_system_file_path = _T("");
	m_fusion_batch_path = _T("");
	m_fusion_use_remover = 1;

	m_type_mapping_enabled = FALSE;
	m_mac_window_mode = FALSE;
	m_show_labels = FALSE;
	m_use_partial_names = FALSE;
	m_disable_autofloppy = FALSE;

}

HFONT CHFVExplorerApp::get_font()
{
	return(m_hfont);
}

CHFVExplorerApp::~CHFVExplorerApp()
{
 StorageUi::state().wait=nullptr;StorageUi::state().thread=0;
	kill_childs();
	floppy_module_global_final();
	SaveSettings();
	if(m_hfont) {
		DeleteObject( m_hfont );
		m_hfont = 0;
	}
	tmap_final();
}

void CHFVExplorerApp::WriteIconIndex(void)
{
	WriteProfileInt("Setup","NextIconNumber",m_next_icon_index);
}

void CHFVExplorerApp::LoadSettings(void)
{
	m_dosicons = GetProfileInt("Setup","DosIcons",1);
	m_fat_icon_time_limit = GetProfileInt("Setup","DosIconTimeLimit",0);
	m_mountall = GetProfileInt("Setup","MountAllDisks",1);
	m_startup_floppies = GetProfileInt("Setup","StartupFloppyCheck",1);
	m_startup_cds = GetProfileInt("Setup","StartupCDCheck",1);
	m_startup_hds = GetProfileInt("Setup","StartupHDCheck",1);
	m_copyout_mode = GetProfileInt("Setup","CopyOutMode",6);
	m_copyin_mode = GetProfileInt("Setup","CopyInMode",6);
	m_copyin_appledouble = GetProfileInt("Setup","CopyInAppleDouble",1);
	m_next_icon_index = GetProfileInt("Setup","NextIconNumber",0);

	m_version = GetProfileString("Setup","Version");

	m_show_invisible_mac = GetProfileInt("Setup","ShowInvisibleMac",0);
	m_show_invisible_fat = GetProfileInt("Setup","ShowInvisiblePC",1);
	m_protect_locked_mac = GetProfileInt("Setup","ProtectLockedMac",0);
	m_protect_locked_fat = GetProfileInt("Setup","ProtectLockedPC",0);
	m_mac_window_mode = GetProfileInt("Setup","MacWindowMode",0);
	m_show_labels = GetProfileInt("Setup","ShowLabels",0);


	m_executor_dos_path = GetProfileString("Setup","ExecutorDOSPath");
	m_executor_win32_path = GetProfileString("Setup","ExecutorWin32Path");
	m_vmac_path = GetProfileString("Setup","vMacPath");
	m_vmac_startup_folder_name = GetProfileString("Setup","vMacStartupFolder");
	m_vmac_system_file_path = GetProfileString("Setup","vMacSystemFile");

	m_default_emulator = GetProfileInt("Setup","DefaultEmulator",5);
	m_link_name_style = GetProfileInt("Setup","LinkNameStyle",2);
	m_launch_method = GetProfileInt("Setup","vMacLaunchMethod",0);
	m_use_remover = GetProfileInt("Setup","vMacUseRemover",1);

	m_shapeshifter_path = GetProfileString("Setup","ShapeshifterPath");
	m_shapeshifter_startup_folder_name = GetProfileString("Setup","ShapeshifterStartupFolder");
	m_shapeshifter_system_file_path = GetProfileString("Setup","ShapeshifterSystemFile");
	m_shapeshifter_preferences_file_path = GetProfileString("Setup","ShapeshifterPreferences");
	m_shapeshifter_amiga_volume_name = GetProfileString("Setup","ShapeshifterVolume");
	m_shapeshifter_use_remover = GetProfileInt("Setup","ShapeshifterUseRemover",1);

	m_fusion_startup_folder_name = GetProfileString("Setup","FusionStartupFolder");
	m_fusion_system_file_path = GetProfileString("Setup","FusionSystemFile");
	m_fusion_batch_path = GetProfileString("Setup","FusionBatchFile");
	m_fusion_use_remover = GetProfileInt("Setup","FusionUseRemover",1);

	point_size = GetProfileInt("Setup","PointSize",10);
	CString f;
	f = GetProfileString("Setup","Font");
	if(f == "") {
		lstrcpy( font_name, "Arial" );
	} else {
		lstrcpy( font_name, (LPCSTR)f );
	}
	m_is_font_sjis = is_font_sjis = GetProfileInt("Setup","IsFontSjis",0);/* noda */
	set_font( font_name, point_size );

	m_type_mapping_enabled = GetProfileInt("Setup","TypeMappingEnabled",1);

	m_use_partial_names = GetProfileInt("Setup","UsePartialNames",0);
	set_invalid_cd_hack( m_use_partial_names );

	m_disable_autofloppy = GetProfileInt("Setup","DisableAutoFloppy",1);

	m_do_harddisks = GetProfileInt("Setup","EnableMacHardDisksAtYourOwnRisk",0);
	if(m_do_harddisks) {
		int warn = GetProfileInt("Setup","WarnBeforeHardDiskUse",1);
		if(warn) {
			AfxMessageBox( "You have enabled the Mac hard disk access. Be careful, It is NOT properly tested!" );
			WriteProfileInt("Setup","WarnBeforeHardDiskUse",0);
		}
	} else {
		WriteProfileString("Setup","WarnBeforeHardDiskUse",NULL);
	}
}

void CHFVExplorerApp::SaveSettings(void)
{
	WriteProfileInt("Setup","DosIcons",m_dosicons);
	WriteProfileInt("Setup","DosIconTimeLimit",m_fat_icon_time_limit);
	WriteProfileInt("Setup","MountAllDisks",m_mountall);
	WriteProfileInt("Setup","StartupFloppyCheck",m_startup_floppies);
	WriteProfileInt("Setup","StartupCDCheck",m_startup_cds);
	WriteProfileInt("Setup","StartupHDCheck",m_startup_hds);
	WriteProfileInt("Setup","CopyOutMode",m_copyout_mode);
	WriteProfileInt("Setup","CopyInMode",m_copyin_mode);
	WriteProfileInt("Setup","CopyInAppleDouble",m_copyin_appledouble);
	WriteIconIndex();

	WriteProfileString("Setup","Version",m_version);

	WriteProfileInt("Setup","ShowInvisibleMac",m_show_invisible_mac);
	WriteProfileInt("Setup","ShowInvisiblePC",m_show_invisible_fat);
	WriteProfileInt("Setup","ProtectLockedMac",m_protect_locked_mac);
	WriteProfileInt("Setup","ProtectLockedPC",m_protect_locked_fat);

	WriteProfileInt("Setup","MacWindowMode",m_mac_window_mode);
	WriteProfileInt("Setup","ShowLabels",m_show_labels);

	WriteProfileString("Setup","ExecutorDOSPath",m_executor_dos_path);
	WriteProfileString("Setup","ExecutorWin32Path",m_executor_win32_path);
	WriteProfileString("Setup","vMacPath",m_vmac_path);
	WriteProfileString("Setup","vMacStartupFolder",m_vmac_startup_folder_name);
	WriteProfileString("Setup","vMacSystemFile",m_vmac_system_file_path);

	WriteProfileInt("Setup","DefaultEmulator",m_default_emulator);

	WriteProfileInt("Setup","LinkNameStyle",m_link_name_style);
	WriteProfileInt("Setup","vMacLaunchMethod",m_launch_method);
	WriteProfileInt("Setup","vMacUseRemover",m_use_remover);

	WriteProfileString("Setup","ShapeshifterPath",m_shapeshifter_path);
	WriteProfileString("Setup","ShapeshifterStartupFolder",m_shapeshifter_startup_folder_name);
	WriteProfileString("Setup","ShapeshifterSystemFile",m_shapeshifter_system_file_path);
	WriteProfileString("Setup","ShapeshifterPreferences",m_shapeshifter_preferences_file_path);
	WriteProfileString("Setup","ShapeshifterVolume",m_shapeshifter_amiga_volume_name);
	WriteProfileInt("Setup","ShapeshifterUseRemover",m_shapeshifter_use_remover);

	WriteProfileString("Setup","FusionStartupFolder",m_fusion_startup_folder_name);
	WriteProfileString("Setup","FusionSystemFile",m_fusion_system_file_path);
	WriteProfileString("Setup","FusionBatchFile",m_fusion_batch_path);
	WriteProfileInt("Setup","FusionUseRemover",m_fusion_use_remover);

	WriteProfileInt("Setup","PointSize",point_size);
	WriteProfileString("Setup","Font",CString(font_name));
	WriteProfileInt("Setup","IsFontSjis",m_is_font_sjis);/* noda */

	WriteProfileInt("Setup","TypeMappingEnabled",m_type_mapping_enabled);

	WriteProfileInt("Setup","UsePartialNames",m_use_partial_names);

	// WriteProfileInt("Setup","EnableMacHardDisksAtYourOwnRisk",m_do_harddisks);
}

void CHFVExplorerApp::CheckDefaultSettings(void)
{
	char path[_MAX_PATH];

	if(m_executor_dos_path == "") {
		::GetPrivateProfileString( 
			"Setup", "Executor path", "", path, MAX_PATH, "execut95.ini"
		);
		m_executor_dos_path = CString(path);
	}

	if(m_executor_win32_path == "") {
		::GetPrivateProfileString( 
			"Setup", "Executor path", "", path, MAX_PATH, "execut95.ini"
		);
		m_executor_win32_path = CString(path);
	}

	if(m_vmac_startup_folder_name == "") {
		m_vmac_startup_folder_name = CString("System Folder:Startup Items");
	}

	if(m_shapeshifter_startup_folder_name == "") {
		m_shapeshifter_startup_folder_name = CString("System Folder:Startup Items");
	}
	if(m_shapeshifter_amiga_volume_name == "") {
		m_shapeshifter_amiga_volume_name = CString("Executor:");
	}

	if(m_fusion_startup_folder_name == "") {
		m_fusion_startup_folder_name = CString("System Folder:Startup Items");
	}
}

int CHFVExplorerApp::ExitInstance(){BrowserTransfers::Clear();ShutdownStorageUi();return CWinApp::ExitInstance();}

BOOL CHFVExplorerApp::InitInstance()
{
 auto windows=WindowsCompatibility::Current();
 if(__argc==3&&!strcmp(__argv[1],"--compatibility-report")){
  FILE* output=nullptr;if(fopen_s(&output,__argv[2],"w")||!output)ExitProcess(2);
  WindowsCompatibility::Report(output,windows);fclose(output);ExitProcess(WindowsCompatibility::Supported(windows)?0:1);
 }
 if(!WindowsCompatibility::Supported(windows)){AfxMessageBox(WindowsCompatibility::Requirement(),MB_OK|MB_ICONERROR);return FALSE;}

#ifdef HFV_TESTING
 extern int RunBackgroundMetadataTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-background-metadata"))ExitProcess(RunBackgroundMetadataTest(__argv[2]));
 extern int RunBackgroundMutationsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-background-mutations"))ExitProcess(RunBackgroundMutationsTest(__argv[2]));
 extern int RunBackgroundWindowCloseTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-background-window-close"))ExitProcess(RunBackgroundWindowCloseTest(__argv[2]));
 extern int RunIndependentStorageTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-independent-storage"))ExitProcess(RunIndependentStorageTest(__argv[2]));
 extern int RunFloppyCopyRegressionsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-floppy-copy-regressions"))ExitProcess(RunFloppyCopyRegressionsTest(__argv[2]));
 extern int RunBackgroundCopiesTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-background-copies"))ExitProcess(RunBackgroundCopiesTest(__argv[2]));
 extern int RunBrowserTransfersTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-browser-transfers"))ExitProcess(RunBrowserTransfersTest(__argv[2]));
 extern int RunBrowserWindowsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-browser-windows"))ExitProcess(RunBrowserWindowsTest(__argv[2]));
 extern int RunVolumeCoordinationTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-volume-coordination"))ExitProcess(RunVolumeCoordinationTest(__argv[2]));
 extern int RunFileOperationsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-file-operations"))ExitProcess(RunFileOperationsTest(__argv[2]));
 extern int RunBrowserSessionTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-browser-sessions"))ExitProcess(RunBrowserSessionTest(__argv[2]));
 extern int RunSharedBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-shared-browser"))ExitProcess(RunSharedBrowserTest(__argv[2]));
 extern int RunExclusiveAccessTest(const char*);
 extern int RunExclusiveAccessChild(const char*,const char*,const char*,const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-storage-exclusive"))ExitProcess(RunExclusiveAccessTest(__argv[2]));
 if(__argc==4&&!strcmp(__argv[1],"--exclusive-attempt"))ExitProcess(RunExclusiveAccessChild(__argv[2],__argv[3],nullptr,nullptr));
 if(__argc==6&&!strcmp(__argv[1],"--exclusive-hold"))ExitProcess(RunExclusiveAccessChild(__argv[2],__argv[3],__argv[4],__argv[5]));
 extern int RunMainMacBrowserTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-main-mac-browser")==0)ExitProcess(RunMainMacBrowserTest(__argv[2]));
 extern int RunFileMutationTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-file-mutations")==0)ExitProcess(RunFileMutationTest(__argv[2]));
 extern int RunHfsPlusCdTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-hfsplus-cd")==0)ExitProcess(RunHfsPlusCdTest(__argv[2]));
 extern int RunHfsPlusUiTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-hfsplus-ui")==0)ExitProcess(RunHfsPlusUiTest(__argv[2]));
 extern int RunMediaControlTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-media-control")==0)ExitProcess(RunMediaControlTest(__argv[2]));
 extern int RunFloppyMenuTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-floppy-menu")==0)ExitProcess(RunFloppyMenuTest(__argv[2]));
 extern int RunFindFilesTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-find-files")==0)ExitProcess(RunFindFilesTest(__argv[2]));
 extern int RunCdBurnTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-cd-burn")==0)ExitProcess(RunCdBurnTest(__argv[2]));
 extern int RunCdImageTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-cd-image")==0)ExitProcess(RunCdImageTest(__argv[2]));
 extern int RunCopyProgressTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-copy-progress")==0)ExitProcess(RunCopyProgressTest(__argv[2]));
 extern int RunDeviceWaitGuiTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-device-wait")==0)ExitProcess(RunDeviceWaitGuiTest(__argv[2]));
 extern int RunLargeImageGuiTest(const char*,const char*);
 extern int RunFloppyGuiTest(const char*,const char*);
 if(__argc==4 && strcmp(__argv[1],"--verify-floppy")==0) ExitProcess(RunFloppyGuiTest(__argv[2],__argv[3]));
 // No settings, CD discovery, window creation, or GUI event loop in test mode.
 if(__argc==4 && strcmp(__argv[1], "--verify-large-image")==0)
  ExitProcess(RunLargeImageGuiTest(__argv[2],__argv[3]));
 ExitProcess(2);
#endif
 InitializeStorageUi();
 // Keep this experimental port's preferences separate from the historical app.
 char settingsPath[MAX_PATH];
 GetModuleFileName(NULL, settingsPath, MAX_PATH);
 char *filename = strrchr(settingsPath, '\\');
 if(filename) strcpy(filename + 1, "HFVExplorer-x64.ini");
 free((void*)m_pszProfileName);
 m_pszProfileName = _strdup(settingsPath);
	LoadSettings();
	CheckDefaultSettings();

	m_prevVersion = m_version;
 m_startup_hds = m_do_harddisks = FALSE;

	if(m_version != "2.0.0") {
		m_version = "2.0.0";
		WriteProfileString("Setup","Version",m_version);
	}
	SaveSettings();

	HFSIFACE_set_copy_modes( m_copyout_mode, m_copyin_mode, m_copyin_appledouble );

	floppy_module_global_init();

#ifdef _AFXDLL
	Enable3dControls();
#else
	Enable3dControlsStatic();
#endif

	LoadStdProfileSettings(10);

 InstallBrowserTemplate();

	EnableShellOpen();
	// File associations are deliberately not registered by this portable build.

	CHFVCommandLineInfo cmdInfo;
	ParseCommandLine(cmdInfo);

	// __asm int 3

	if (!ProcessShellCommand(cmdInfo)) return FALSE;

	m_pMainWnd->DragAcceptFiles();

	tmap_init();

	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// CAboutDlg dialog used for App About

class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

// Dialog Data
	//{{AFX_DATA(CAboutDlg)
	enum { IDD = IDD_ABOUTBOX };
	//}}AFX_DATA

	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CAboutDlg)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:
	//{{AFX_MSG(CAboutDlg)
		// No message handlers
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
	//{{AFX_DATA_INIT(CAboutDlg)
	//}}AFX_DATA_INIT
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CAboutDlg)
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
	//{{AFX_MSG_MAP(CAboutDlg)
		// No message handlers
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

// App command to run the dialog
void CHFVExplorerApp::OnAppAbout()
{
	CAboutDlg aboutDlg;
	aboutDlg.DoModal();
}

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerApp commands

#define CHILDMAGIC 0x12345678
static UINT our_child_registration = RegisterWindowMessage("HFVRegisterChild");

BOOL CHFVExplorerApp::OnIdle(LONG lCount) 
{
	int x, y, cx, cy;
	UINT flags;
	CWnd *insertafter;

	if(m_opendir != "") {
		int pc = m_opendir.Mid(1,2) == ":\\";
		// __asm int 3
		m_doc->my_chdir( m_opendir, !pc );
		m_opendir = "";
		// m_main->ActivateFrame( SW_SHOW );
		x = 0;
		y = 0;
		cx = 0;
		cy = 0;
		if(is_desktop_application) {
			insertafter = 0;
			flags = SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW |SWP_NOZORDER;
		} else {
			insertafter = (CWnd *)&CWnd::wndTopMost;
			insertafter = (CWnd *)0;
			flags = SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW;

			CHFVExplorerApp	*pApp;
			pApp = (CHFVExplorerApp *)AfxGetApp();
			LPARAM w;
			// __asm int 3
			w = (LPARAM)pApp->m_main->m_hWnd;
			::PostMessage( m_parent_hwnd, our_child_registration, CHILDMAGIC, (LPARAM)w );
		}
		m_main->SetWindowPos( insertafter, x, y, cx, cy, flags );
		m_startdir_processed = 1;
		if(is_desktop_application) {
			m_main->ShowWindow(SW_SHOWMAXIMIZED);
		}
		SetForegroundWindow(m_main->m_hWnd);
	} else if(!m_startdir_processed) {
		/*
		CString dir;
		int mac;
		m_startdir_processed = 1;
		if(is_desktop_application) {
			get_last_directory( &dir, &mac );
			if(dir != "") my_chdir( dir, mac );
		}
		*/
	}
	
	return CWinApp::OnIdle(lCount);
}

void CHFVExplorerApp::add_child( HWND hi ) 
{
	child_array.Add( (DWORD_PTR) hi );
}

void CHFVExplorerApp::kill_childs() 
{
	int i, count = child_array.GetSize();
	HWND w;

	for(i=0; i<count; i++) {
		w = (HWND)child_array.GetAt(i);
		::PostMessage( w, WM_CLOSE, 0, 0 );
	}
}
/*
	get_last_directory( &dir, &mac );

	int mac;
	CString dir
	if(is_desktop_application) {
		dir = get_directory( 0, &mac );
		save_last_directory( dir, mac );
*/

void CHFVExplorerApp::OnFileOpen() 
{
	CString name = "";
 auto owner=m_main;

	CFileDialog dlg( TRUE, _T("DSK"), name,
				OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
				_T("Mac disk images|*.dsk;*.hf*;*.img;*.image;*.mfs;*.dc42|All Files|*.*||"), owner );
	if(dlg.DoModal() == IDOK) {
		name = dlg.GetPathName();
		if(owner)owner->GetBrowserSession().Document()->OnOpenDocument(name);else OpenDocumentFile(name);
	}
}

BOOL CHFVExplorerApp::EnableTimer( BOOL onoff )
{
	BOOL prev = m_timer_enabled;
	m_timer_enabled = onoff;
	return(prev);
}
