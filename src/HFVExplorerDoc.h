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

#ifndef _DOC_INCLUDED
#define _DOC_INCLUDED

#include "MacTypes.h"
#include "itemtype.h"
#include "BrowserDirectory.h"
#include "bndl.h"
#include "BrowserSession.h"
#include "BrowserFolderReader.h"

// #define USE_FILE_MAPPING

#include "mactypes.h"
#include "CFATVolume.h"


#define ID_GOTO_PARENT 0x12345678

namespace BrowserNavigation { struct Request; }
class CHFVExplorerDoc : public CDocument
{
 friend class MainMacBrowser;
 friend class BrowserSession;
 std::unique_ptr<BrowserSession> m_browserSession;
 bool browserOpened_=false;
 void BuildBrowserRoots();
 bool NavigateNamedClassicFolder(const std::string& path,int first=0,int preferred=-1);
 bool NavigateInitialFolder(int firstClassic=0);
protected: // create from serialization only
#ifdef HFV_TESTING
    std::unique_ptr<BrowserFolderReader> folderReader_;
#endif
    BrowserFolderReader::Options FolderOptions() const;
	CHFVExplorerDoc(bool discover = true);
	DECLARE_DYNCREATE(CHFVExplorerDoc)
	void OnCleanDocument();
	void set_floppy_volume_dirty( char *path );

	BOOL find_volume_MDB();
	void hfs_init();


    bool NavigateStartupDirectory(const std::string& path,bool mac);
    bool NavigateClassicFolder(int index, uint32_t id, bool refresh = false,
        const BrowserNavigation::Request* find = nullptr, bool useKnownResource = false);
    void PublishClassicFolder(int index, HTREEITEM item, const std::optional<std::wstring>& parent,
                              const BrowserDirectory::Snapshot&);
    bool NavigateWindowsFolder(int index, const std::wstring& path, bool refresh = false, const BrowserNavigation::Request* find = nullptr);
    bool NavigateWindowsFolder(int index, const std::string& path, bool refresh = false, const BrowserNavigation::Request* find = nullptr);
    void PublishWindowsFolder(int index, const BrowserDirectory::Snapshot&,HTREEITEM item);
#ifdef HFV_TESTING
	void open_sub_directory( int volinx, unsigned long id, CString partial_name );
	void open_fat_directory( int volinx, LPSTR path, int list );
#endif
	int insert_volume( CString name, int is_floppy, int is_cd, int is_hd, int is_removable );
	void enum_hfs_volumes(LPSTR mask);
	void enum_volumes_ext( LPSTR mask, LPSTR ext );
	void open_parent_directory( unsigned long id );
	CString get_directory( HTREEITEM hItem, int *pmac );
	void my_chdir( CString path, int mac, int preferredVolume = -1 );
	void set_current_hitem( HTREEITEM hItem );
	HTREEITEM get_current_hitem();

	// FAT stuff
	HICON get_fat_icon( UINT type );
	int insert_fat_volume( char letter, UINT type );
	void enum_fat_volumes();

	void update_invisible_attributes();

// Attributes
public:
 BrowserSession& GetBrowserSession()const{return *m_browserSession;}
 BrowserMountInventory& Mounts() const {return GetBrowserSession().Mounts();}
	int m_bits_per_pixel;
	int m_mac_show_invisibles;
	int m_pc_show_invisibles;

// Operations
public:

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CHFVExplorerDoc)
	public:
	virtual BOOL OnNewDocument();
	virtual void Serialize(CArchive& ar);
	virtual BOOL OnOpenDocument(LPCTSTR lpszPathName);
	virtual void OnCloseDocument();
 void FinishBackgroundClose();
	//}}AFX_VIRTUAL

// Implementation
public:
	virtual ~CHFVExplorerDoc();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

protected:

// Generated message map functions
protected:
	//{{AFX_MSG(CHFVExplorerDoc)
	afx_msg void OnViewRefresh();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
#endif
