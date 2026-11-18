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

#ifndef _LISTVIEW_INCLUDED
#define _LISTVIEW_INCLUDED

// #define PARENTIDFLAG 0xFFFF8000ul

#include "ftypes.h"
#include "HostOffsets.h"
#include "BrowserDirectory.h"

typedef struct {
	int type;
	long id;
	int volinx;
	int isdir;
	char path[MAX_PATH];
	HTREEITEM htree;
	HICON icon16;
	HICON icon32;
	unsigned long creation_date_lo, creation_date_hi;
	unsigned long modification_date_lo, modification_date_hi;
} list_lparam_struct;

#define FIELD_NAME		0
#define FIELD_SIZE		1
#define FIELD_CREATED 2
#define FIELD_MODIFIED 3
#define FIELD_TYPE		4
#define FIELD_CREATOR 5
#define FIELD_LABEL		6

/////////////////////////////////////////////////////////////////////////////
// CHFVExplorerListView view

class BrowserSession;
namespace BrowserTransfers {class Drag;}
class CHFVExplorerListView : public CListView
{
 BrowserSession* m_browserSession=nullptr;
 std::unique_ptr<BrowserTransfers::Drag> m_browserDrag;
 struct DragIcon {int index;FileOperations::Location location;CPoint position;};
 std::vector<DragIcon> m_dragIcons;
 CPoint m_dragStart;
 LPARAM m_parentRowData=0;
 void CaptureDragIcons();
 void PlaceDraggedIcons(CSize);
 void UpdateDragModifiers();
 void FinishDragImage();
 void CancelBrowserDrag();
 afx_msg void OnCaptureChanged(CWnd*);
 afx_msg void OnCancelMode();
 BOOL PreTranslateMessage(MSG*) override;
public:
 BrowserSession& GetBrowserSession()const;
 BrowserSession* BrowserBinding()const{return m_browserSession;}
 void BindBrowserSession(BrowserSession* session){m_browserSession=session;}

 friend class MainMacBrowser;
 friend class BrowserSession;
 std::map<LPARAM,BrowserDirectory::Row> m_browser_rows;
 MacBrowser::Capabilities m_directory_capabilities;
 bool m_directory_capabilities_known=false;
protected:
	CHFVExplorerListView();           // protected constructor used by dynamic creation
	DECLARE_DYNCREATE(CHFVExplorerListView)

	// palette stuff
	enum { red = 1, green = 2, blue = 3 };

	RGBQUAD m_rgbPalette[256];
	CPalette m_Palette;
	CPalette* m_pOldPalette;

	CPalette* GetPalette();
	void CreatePalette();

	CListCtrl *m_cl;
	CPointerWordArray *m_pointer_array;
	// COLORREF m_bkcolor;
	CImageList* m_drag_list;
	int m_dragging;
	CSize m_sizeDelta;

	int m_editing_item;
	char m_editing_item_name[_MAX_PATH];

	HCURSOR m_move_cursor, m_copy_cursor;

	int m_sorted_by_column;
	BOOL m_sort_is_ascending;
	POINT m_mac_origo;

// Attributes
public:
    bool IsDragging() const { return m_dragging!=0; }
	void on_properties( int inx );
	void on_volume_changed( int volinx );
	void _OnPaletteChanged(CWnd* pFocusWnd);
	BOOL _OnQueryNewPalette();

	int do_rename( 
		int item, 
		char *old_name,
		char *new_name,
		int check_lock
	);
	BOOL new_folder(void);
	void set_cursor_by_type( int control );
	void update_floppy_view( char *volpath );

	int new_bowser_on( CString start_dir );
	void update_current_view();
	int get_inx_type(int inx);
	int get_inx_isdir(int inx);
	void try_delete_selected_items( BOOL confirm );

	void OnButtonUp(CPoint point);
	int FillList();
	int Initialize();
	void set_style( long style );
	CImageList *m_pimagelist;
	CImageList *m_pimagelistSmall;
	int m_item_count;
	int list_clear();
	int initialize_volume( CString root_name, unsigned long id );
    void SetBrowserCapabilities(MacBrowser::Capabilities caps){m_directory_capabilities=caps;m_directory_capabilities_known=true;}
    bool BrowserDirectoryWritable()const{return !m_directory_capabilities_known||m_directory_capabilities.create;}
    bool BrowserSelectionAllows(bool rename)const;
    bool StorageSelectionAvailable(bool exclusive=false)const;
    int InsertBrowserRow(const BrowserDirectory::Row&,HTREEITEM);
    void SortBrowserRows(int column,bool ascending);
    const BrowserDirectory::Row* BrowserRowAt(int row) const;
    bool IsParentRow(int index)const;
    bool OpenWithDrop(const FileOperations::Location& source,const FileOperations::Location& application,bool mac);
#ifdef HFV_TESTING
    void PlaceSelectedIconsForTest(CSize displacement);
#endif
	int list_insert( 
		LPSTR name, 
		int isdir, 
		hfv_size_t size_in_bytes,
		unsigned long creation_date,
		unsigned long modification_date,
		char *stype,
		char *screator,
		unsigned long id,
		HICON hicon32,
		HICON hicon16,
		HTREEITEM htree,
		int volinx,
		LPPOINT ppoint,
		BOOL is_inited,
		int finder_view_mode,
		int finder_color,
        int storageType = LIST_HFS, int *insertedRow = nullptr
	);
	int list_fat_insert(
			LPSTR name, 
			int isdir, 
			hfv_size_t size_in_bytes,
			FILETIME *creation_date,
			FILETIME *modification_date,
			HICON hicon16,
			HICON hicon32,
			char *stype,
			char *screator,
			LPSTR fullpath,
			HTREEITEM htree,
			int volinx );
	void activate_list_item( int inx, int control, int make_shortcut );
	int initialize_list();

	LPARAM make_lparam( 
		int type,
		int volinx, 
		long id,
		LPSTR path,
		int isdir,
		HTREEITEM htree,
		HICON hicon32,
		HICON hicon16,
		unsigned long creation_date_lo, 
		unsigned long creation_date_hi,
		unsigned long modification_date_lo, 
		unsigned long modification_date_hi
		);
	long get_lparam_id( LPARAM lparam );
	int get_lparam_type( LPARAM lparam );
	int get_lparam_isdir( LPARAM lparam );
	int get_lparam_path( LPARAM lparam, LPSTR lpstr, int maxl );
	HTREEITEM get_lparam_htree( LPARAM lparam );
	HICON get_lparam_icon32( LPARAM lparam );
	HICON get_lparam_icon16( LPARAM lparam );
	int get_lparam_volinx( LPARAM lparam );
	int get_lparam_modification( LPARAM lparam, DWORD *lo, DWORD *hi );
	int get_lparam_creation( LPARAM lparam, DWORD *lo, DWORD *hi );

	int get_path( int inx, CString &doc_name );
	int do_open_with( int document, CString app_name, int mac, CString creator );
	int do_open_with_pc( CString document, CString application );
	int do_open_with_mac( CString document, CString application, CString creator );
	int write_text_file( LPSTR name, LPSTR txt );
	void get_vmac_launcher_name( char *name );
	void add_link_tail( char *lpszPathLink, char *id );
	int check_shortcut_installation( int emulator );
	int do_make_shortcut( int doc_inx, CString application, CString creator, int emulator, int make_shortcut );
	int do_make_shortcut_windows( int doc_inx, CString application, CString creator, int make_shortcut );
	int do_make_shortcut_vmac( int doc_inx, CString application, CString creator, int make_shortcut );
	int do_make_shortcut_e_win32( int doc_inx, CString application, CString creator, int make_shortcut );
	int do_make_shortcut_e_dos( int doc_inx, CString application, CString creator, int make_shortcut );
	int do_make_shortcut_ss( int doc_inx, CString application, CString creator, int make_shortcut );
	int do_make_shortcut_fusion( int doc_inx, CString application, CString creator, int make_shortcut );
	int save_icon( int doc_inx, LPSTR lpszIconFile, int is_doc );
	void get_icons_folder_name( char *name );
	int make_icon_name( LPSTR lpszIconFile );
	int make_e_dos_param_file_name( LPSTR lpszParamsFile );

	void destroy_pointers();

	BOOL map_selected_item( void );

	void do_report_sort(void);

	void set_origo( int x, int y );
	void go_up( int control );

	void save_icon_command( int inx );
	void toggle_label_colors( void );
	void set_label_color( int finder_color );

	void edit_cut_copy( BOOL cutting, BOOL append );
	void edit_copy(void);
	void edit_cut(void);
	void edit_paste(void);
	void edit_copy_append(void);
	void edit_cut_append(void);
	void empty_clipboard( void );
	void edit_select_all(void);



// Operations
public:
	// void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CHFVExplorerListView)
	protected:
	virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam);
	//}}AFX_VIRTUAL

// Implementation
protected:
	virtual ~CHFVExplorerListView();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

	// Generated message map functions
protected:
	//{{AFX_MSG(CHFVExplorerListView)
	afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
	afx_msg void OnSysKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg BOOL OnQueryNewPalette();
	afx_msg void OnPaletteChanged(CWnd* pFocusWnd);
	afx_msg void OnBegindrag(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnItemchanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnEndlabeledit(NMHDR* pNMHDR, LRESULT* pResult);
#ifdef HFV_TESTING
public:
 void TestDeleteSelection(){try_delete_selected_items(FALSE);}
 LRESULT TestCommitLabel(int row,const char* name){LV_DISPINFO edit{};edit.item.iItem=row;LRESULT result=0;OnBeginlabeledit(&edit.hdr,&result);if(result)return -1;edit.item.pszText=const_cast<char*>(name);OnEndlabeledit(&edit.hdr,&result);return result;}
protected:
#endif

	afx_msg void OnBeginlabeledit(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnColumnclick(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnEditLabel();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
#endif
