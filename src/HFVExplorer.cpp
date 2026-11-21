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
#include "NativeDpi.h"
#include "WindowsCompatibility.h"
#include "CopyProcess.h"
#include "VolumeSession.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "HFVExplorer.h"
#include "BrowserTransfers.h"
#include "BrowserSession.h"

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
	//}}AFX_MSG_MAP
	// Standard file based document commands
 ON_COMMAND(ID_FILE_NEW, OnNewBrowser)
 ON_COMMAND(ID_TOOLS_NEW_BROWSER, OnNewBrowser)
 ON_UPDATE_COMMAND_UI(ID_TOOLS_NEW_BROWSER, OnUpdateNewBrowser)
 ON_UPDATE_COMMAND_UI(ID_FILE_NEW, OnUpdateNewBrowser)
 ON_COMMAND(ID_APP_EXIT, OnExitBrowsers)
END_MESSAGE_MAP()


/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerApp construction

CHFVExplorerApp theApp;

// Quick hack from "volume.c"
extern "C" void set_invalid_cd_hack( int onoff );

void CHFVExplorerApp::ClearBrowserFonts()
{
    for (const auto& font : m_browserFonts) DeleteObject(font.second);
    m_browserFonts.clear();
}

CHFVExplorerApp::CHFVExplorerApp()
{

	m_opendir = "";
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
	m_is_font_sjis = FALSE; /* noda */
	m_show_invisible_mac = 0;
	m_show_invisible_fat = 1;
	m_protect_locked_mac = 0;
	m_protect_locked_fat = 0;





	m_type_mapping_enabled = FALSE;
	m_mac_window_mode = FALSE;
	m_show_labels = FALSE;
	m_use_partial_names = FALSE;
	m_disable_autofloppy = FALSE;

}

HFONT CHFVExplorerApp::get_font(UINT dpi)
{
    const auto cached = m_browserFonts.find(dpi);
    if (cached != m_browserFonts.end()) return cached->second;
    const int height = (std::max)(1, NativeDpi::Scale(point_size, dpi));
    const auto font = CreateFont(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        m_is_font_sjis ? SHIFTJIS_CHARSET : ANSI_CHARSET, 0, 0, 0, 0, font_name);
    if (font) m_browserFonts.emplace(dpi, font);
    return font;
}

CHFVExplorerApp::~CHFVExplorerApp()
{
 StorageUi::state().wait=nullptr;StorageUi::state().thread=0;
	floppy_module_global_final();
	SaveSettings();
    for (const auto& font : m_browserFonts) DeleteObject(font.second);
	tmap_final();
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

	m_version = GetProfileString("Setup","Version");

	m_show_invisible_mac = GetProfileInt("Setup","ShowInvisibleMac",0);
	m_show_invisible_fat = GetProfileInt("Setup","ShowInvisiblePC",1);
	m_protect_locked_mac = GetProfileInt("Setup","ProtectLockedMac",0);
	m_protect_locked_fat = GetProfileInt("Setup","ProtectLockedPC",0);
	m_mac_window_mode = GetProfileInt("Setup","MacWindowMode",0);
	m_show_labels = GetProfileInt("Setup","ShowLabels",0);






	point_size = GetProfileInt("Setup","PointSize",10);
	CString f;
	f = GetProfileString("Setup","Font");
	if(f == "") {
		lstrcpy( font_name, "Arial" );
	} else {
		lstrcpy( font_name, (LPCSTR)f );
	}
	m_is_font_sjis = is_font_sjis = GetProfileInt("Setup","IsFontSjis",0);/* noda */
	ClearBrowserFonts();

	m_type_mapping_enabled = GetProfileInt("Setup","TypeMappingEnabled",1);

	m_use_partial_names = GetProfileInt("Setup","UsePartialNames",0);
	set_invalid_cd_hack( m_use_partial_names );

	m_disable_autofloppy = GetProfileInt("Setup","DisableAutoFloppy",1);


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

	WriteProfileString("Setup","Version",m_version);

	WriteProfileInt("Setup","ShowInvisibleMac",m_show_invisible_mac);
	WriteProfileInt("Setup","ShowInvisiblePC",m_show_invisible_fat);
	WriteProfileInt("Setup","ProtectLockedMac",m_protect_locked_mac);
	WriteProfileInt("Setup","ProtectLockedPC",m_protect_locked_fat);

	WriteProfileInt("Setup","MacWindowMode",m_mac_window_mode);
	WriteProfileInt("Setup","ShowLabels",m_show_labels);






	WriteProfileInt("Setup","PointSize",point_size);
	WriteProfileString("Setup","Font",CString(font_name));
	WriteProfileInt("Setup","IsFontSjis",m_is_font_sjis);/* noda */

	WriteProfileInt("Setup","TypeMappingEnabled",m_type_mapping_enabled);

	WriteProfileInt("Setup","UsePartialNames",m_use_partial_names);

}

int CHFVExplorerApp::ExitInstance(){BrowserTransfers::Clear();ShutdownStorageUi();return CWinApp::ExitInstance();}

BOOL CHFVExplorerApp::InitInstance()
{
 if(__argc==4&&!strcmp(__argv[1],"--volume-session"))ExitProcess(VolumeSession::Child(__argv[2],__argv[3]));
 if(__argc==4&&!strcmp(__argv[1],"--copy-worker"))ExitProcess(CopyProcess::Child(__argv[2],__argv[3]));
 auto windows=WindowsCompatibility::Current();
 if(__argc==3&&!strcmp(__argv[1],"--compatibility-report")){
  FILE* output=nullptr;if(fopen_s(&output,__argv[2],"w")||!output)ExitProcess(2);
  WindowsCompatibility::Report(output,windows);fclose(output);ExitProcess(WindowsCompatibility::Supported(windows)?0:1);
 }
 if(!WindowsCompatibility::Supported(windows)){AfxMessageBox(WindowsCompatibility::Requirement(),MB_OK|MB_ICONERROR);return FALSE;}

#ifdef HFV_TESTING
 extern int RunClassicRecoveryTest(const char*);
 extern int RunClassicSiblingRecoveryTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-classic-sibling-recovery"))ExitProcess(RunClassicSiblingRecoveryTest(__argv[2]));
 extern int RunClassicRecoveryChild(const char*,unsigned,const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-classic-recovery"))ExitProcess(RunClassicRecoveryTest(__argv[2]));
 if(__argc==5&&!strcmp(__argv[1],"--classic-recovery-child"))ExitProcess(RunClassicRecoveryChild(__argv[2],strtoul(__argv[3],nullptr,10),__argv[4]));
 extern int RunTypedLocationsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-typed-locations"))ExitProcess(RunTypedLocationsTest(__argv[2]));
 extern int RunCopyReleaseTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-copy-release"))ExitProcess(RunCopyReleaseTest(__argv[2]));
 extern int RunCopyParentExitTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-copy-parent-exit"))ExitProcess(RunCopyParentExitTest(__argv[2]));
 extern int RunCopyProcessTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-copy-process"))ExitProcess(RunCopyProcessTest(__argv[2]));
 extern int RunWindowsPathCoordinationTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-windows-path-coordination"))ExitProcess(RunWindowsPathCoordinationTest(__argv[2]));
 extern int RunPerVolumeCommandsTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-per-volume-commands"))ExitProcess(RunPerVolumeCommandsTest(__argv[2]));
 extern int RunPhysicalRemovableTest(const char*);
 extern int RunNativeFloppyMoveTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--native-floppy-move-test"))ExitProcess(RunNativeFloppyMoveTest(__argv[2]));
 extern int RunNativeFloppyRecoveryCheck(const char*,const char*);
 if(__argc==4&&!strcmp(__argv[1],"--native-floppy-recovery-check"))
  ExitProcess(RunNativeFloppyRecoveryCheck(__argv[2],__argv[3]));
 extern int RunRemovableRecoveryTest(const char*);
 extern int RunRemovableRecoveryChild(const char*,const char*,unsigned);
 if(__argc==3&&!strcmp(__argv[1],"--verify-removable-recovery"))ExitProcess(RunRemovableRecoveryTest(__argv[2]));
 if(__argc==5&&!strcmp(__argv[1],"--removable-recovery-child"))ExitProcess(RunRemovableRecoveryChild(__argv[2],__argv[3],strtoul(__argv[4],nullptr,10)));
 if(__argc==3&&!strcmp(__argv[1],"--verify-physical-removable"))ExitProcess(RunPhysicalRemovableTest(__argv[2]));
 extern int RunHardLinksBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-hard-links"))ExitProcess(RunHardLinksBrowserTest(__argv[2]));
 extern int RunFolderMetadataTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-folder-metadata"))ExitProcess(RunFolderMetadataTest(__argv[2]));
 extern int RunOtherHfsPlusBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-other-hfsplus"))ExitProcess(RunOtherHfsPlusBrowserTest(__argv[2]));
 extern int RunCompressionWriteBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-compression-write"))ExitProcess(RunCompressionWriteBrowserTest(__argv[2]));
 extern int RunOpticalDataTrackTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-optical-data-tracks"))ExitProcess(RunOpticalDataTrackTest(__argv[2]));
 extern int RunMfsWriteBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-mfs-write-browser"))ExitProcess(RunMfsWriteBrowserTest(__argv[2]));
 extern int RunExtendedAttributesTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-extended-attributes"))ExitProcess(RunExtendedAttributesTest(__argv[2]));
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
 extern int RunBrowserPreviewTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-browser-preview"))ExitProcess(RunBrowserPreviewTest(__argv[2]));
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
 extern int RunFolderCacheTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-folder-cache"))ExitProcess(RunFolderCacheTest(__argv[2]));
 extern int RunMainMacBrowserTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-main-mac-browser")==0)ExitProcess(RunMainMacBrowserTest(__argv[2]));
 extern int RunFileMutationTest(const char*);
 if(__argc==3 && strcmp(__argv[1],"--verify-file-mutations")==0)ExitProcess(RunFileMutationTest(__argv[2]));
 extern int RunEncryptedDmgTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-encrypted-dmg"))ExitProcess(RunEncryptedDmgTest(__argv[2]));
 extern int RunSparseBundleTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-sparse-bundle"))ExitProcess(RunSparseBundleTest(__argv[2]));
 extern int RunSparseDiskTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-sparse-image"))ExitProcess(RunSparseDiskTest(__argv[2]));
 extern int RunNdifImageTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-ndif-image"))ExitProcess(RunNdifImageTest(__argv[2]));
 extern int RunDmgImageTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-dmg-image"))ExitProcess(RunDmgImageTest(__argv[2]));
 extern int RunApfsContainerTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-apfs-container"))ExitProcess(RunApfsContainerTest(__argv[2]));
 extern int RunOpticalBrowserTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-optical-browser"))ExitProcess(RunOpticalBrowserTest(__argv[2]));
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
 extern int RunRawOpticalTest(const char*);
 if(__argc==3&&!strcmp(__argv[1],"--verify-raw-optical"))ExitProcess(RunRawOpticalTest(__argv[2]));
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

	m_prevVersion = m_version;

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

	tmap_init();
	if(auto frame=ActiveBrowser())OpenStartupDirectory(frame->GetBrowserSession());

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


void CHFVExplorerApp::OpenStartupDirectory(BrowserSession& session)
{
 if(m_opendir.IsEmpty())return;
 const CString directory=m_opendir;
 m_opendir.Empty();
 auto frame=session.Owner();
 const bool windows=directory.Mid(1,2)==":\\";
 session.Document()->NavigateStartupDirectory((LPCSTR)directory,!windows);
 frame->SetWindowPos(nullptr,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW|SWP_NOZORDER);
 frame->ShowWindow(SW_SHOWMAXIMIZED);
 frame->SetForegroundWindow();
}

BOOL CHFVExplorerApp::EnableTimer( BOOL onoff )
{
	BOOL prev = m_timer_enabled;
	m_timer_enabled = onoff;
	return(prev);
}
