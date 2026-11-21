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

#ifndef _MAINFRM_DEFINED
#define _MAINFRM_DEFINED
#include "HostOffsets.h"
#include "NativeDpi.h"
#include <memory>
#include <vector>

int ask_new_volume( char *volpath, char *vname, hfv_size_t *psize, int *normal, int *floppyDrive = nullptr );

#define MYTIMERID 123

class LauncherWriteBack;
struct MediaControlTarget;
class BrowserSession;

class BrowserSplitter : public CSplitterWnd {
public:
    void ApplyDpi(UINT from, UINT to);
protected:
    UINT dpi_ = NativeDpi::Base;
private:
    NativeDpi::Measurement firstPane_;
};

class PreviewSplitter : public BrowserSplitter {
public:
    void ShowPreview(CWnd&, bool);
    void RecalcLayout() override;
};

class BrowserToolbar : public CToolBar {
public:
    void ApplyDpi(UINT dpi);
protected:
    LRESULT WindowProc(UINT,WPARAM,LPARAM) override;
private:
    UINT dpi_ = NativeDpi::Base;
};

class CMainFrame : public CFrameWnd
{
 BrowserSession* m_browserSession=nullptr;
 bool m_restoreMaximized=false;
 UINT m_dpi=NativeDpi::Base;
 NativeDpi::Measurement m_statusWidths[4];
 void ApplyDpi(UINT, const RECT*);
protected: // create from serialization only
	BrowserSplitter m_wndSplitter;
	PreviewSplitter m_wndSplitter2;

	CMainFrame();
	DECLARE_DYNCREATE(CMainFrame)

// Attributes
public:
 BrowserSession& GetBrowserSession()const;
 bool CommandAllowed(UINT)const;
	void OnChildRegistration(WPARAM wParam, LPARAM lParam);

// Operations
public:
	void OnProgramProperties() ;

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CMainFrame)
	public:
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
 void ActivateFrame(int show=-1) override;
 void OnUpdateFrameTitle(BOOL addToTitle) override;
	virtual BOOL DestroyWindow();
	protected:
	virtual BOOL OnCreateClient(LPCREATESTRUCT lpcs, CCreateContext* pContext);
	virtual LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);
	//}}AFX_VIRTUAL

// Implementation
public:
	virtual ~CMainFrame();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

	void MountPath( char *name );
	void go_home_dir( void );
 void TrackLaunchedDocument(std::unique_ptr<LauncherWriteBack>);
	virtual void LoadWindowPosition(void);
	virtual void SaveWindowPosition(void);
	void OnCdEjectEjectReload( int reload );
 MediaControlTarget SelectedMediaControlTarget() const;
 BOOL OnCommand(WPARAM,LPARAM) override;
 BOOL PreTranslateMessage(MSG*) override;
 BOOL OnCmdMsg(UINT,int,void*,AFX_CMDHANDLERINFO*) override;
 afx_msg void OnActivate(UINT,CWnd*,BOOL);
 afx_msg void OnFileOpenImage();
 afx_msg void OnFileOpenSparseBundle();
 afx_msg void OnOpenRecentImage(UINT);
 afx_msg void OnUpdateMacTransfer(CCmdUI*);
 afx_msg void OnUpdateMacEdit(CCmdUI*);

protected:  // control bar embedded members
	CStatusBar  m_wndStatusBar;
	BrowserToolbar m_wndToolBar;
	UINT_PTR m_timer;
	BOOL m_volcheck_enabled;
	
 bool m_handlingTimer=false;
 std::vector<std::unique_ptr<LauncherWriteBack>> m_launchedDocuments;

	/*
	char m_waiting_doc_win_name[_MAX_PATH];
	char m_waiting_srcvolpath[_MAX_PATH];
	char m_waiting_srcname[_MAX_PATH];
	CTime m_waiting_emu_mtime;
	LONG m_waiting_emu_size;
	*/


// Generated message map functions
protected:
	//{{AFX_MSG(CMainFrame)
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnViewDetails();
    afx_msg void OnViewPreviewPane();
    afx_msg void OnUpdateViewPreviewPane(CCmdUI*);
	afx_msg void OnViewLargeicons();
	afx_msg void OnViewList();
	afx_msg void OnViewSmallicons();
	afx_msg void OnViewOptions();
	afx_msg void OnActivateApp(BOOL bActive, DWORD hTask);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg BOOL OnQueryNewPalette();
	afx_msg void OnPaletteChanged(CWnd* pFocusWnd);
	afx_msg void OnDropFiles(HDROP hDropInfo);
	afx_msg void OnFileNewvolume();
 afx_msg void OnFileNewHfsPlus();
 afx_msg void OnFileConvertDmg();
	afx_msg void OnFileVolumefromfloppy();
	afx_msg void OnFileVolumetofloppy();
	afx_msg void OnToolsDumpcddebug();
	afx_msg void OnCdEjectReload();
	afx_msg void OnCdEjectEject();
	afx_msg void OnHelp();
	afx_msg void OnViewMacWindow();
	afx_msg void OnEditCopy();
	afx_msg void OnEditCut();
	afx_msg void OnEditPaste();
 afx_msg void OnPasteHardLink();
 afx_msg void OnScanRemovable();
 afx_msg void OnUpdatePasteHardLink(CCmdUI*);
	afx_msg void OnEditCopyAppend();
	afx_msg void OnEditCutAppend();
	afx_msg void OnEditSelectAll();
	afx_msg BOOL OnQueryOpen();
	afx_msg void OnClose();
	afx_msg void OnFileEject();
	afx_msg void OnFileReload();
	afx_msg void OnToolsBurn();
 afx_msg void OnToolsFind();
 afx_msg void OnToolsBasiliskII();
 afx_msg void OnToolsMiniVMac();
 afx_msg void OnToolsApfsSnapshots();
 afx_msg void OnToolsApfsRestore();
 afx_msg void OnToolsApfsRenameVolume();
 afx_msg void OnUpdateApfsRenameVolume(CCmdUI*);
 afx_msg void OnUpdateApfsRestore(CCmdUI*);

 afx_msg void OnUpdatePhysicalMedia(CCmdUI* ui);
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
#endif
