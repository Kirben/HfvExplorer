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
#include "BrowserSession.h"
#include "HFVExplorer.h"
#include "HFVPreviewView.h"
#include "special.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CHFVPreviewView

IMPLEMENT_DYNCREATE(CHFVPreviewView, CView)

CHFVPreviewView::CHFVPreviewView()
{
	memset( &m_lparam, 0, sizeof(m_lparam) );
	m_show = 0;
	m_hfont = CreateFont( 
		-point_size, 0, 0, 0, FW_NORMAL, (BYTE)FALSE, 0, 0, 0, 0, 0, 0, 0, font_name );
	m_lineheight = 13;
}

CHFVPreviewView::~CHFVPreviewView()
{
 if(m_browserSession)m_browserSession->Detach(this);
 auto application=static_cast<CHFVExplorerApp*>(AfxGetApp());if(application->m_pre==this)application->m_pre=nullptr;
	if(m_hfont) {
		DeleteObject( m_hfont );
		m_hfont = 0;
	}
}

void CHFVPreviewView::Initialize()
{
	if(m_hfont) {
		::SendMessage( 
			m_hWnd, 
			WM_SETFONT,
			(WPARAM) m_hfont,
			MAKELPARAM(TRUE,0) );
	}
}

void CHFVPreviewView::set_pane(int index){
 auto& session=GetBrowserSession();CWnd* pane=index==PANE_TREE?static_cast<CWnd*>(session.Tree()):index==PANE_LIST?static_cast<CWnd*>(session.List()):this;
 if(pane&&IsWindow(pane->GetSafeHwnd()))pane->SetFocus();
}
int CHFVPreviewView::get_pane_index(){
 auto& session=GetBrowserSession();HWND focus=::GetFocus();
 if(session.Tree()&&(focus==session.Tree()->GetSafeHwnd()||::IsChild(session.Tree()->GetSafeHwnd(),focus)))return PANE_TREE;
 if(session.List()&&(focus==session.List()->GetSafeHwnd()||::IsChild(session.List()->GetSafeHwnd(),focus)))return PANE_LIST;
 return PANE_PREVIEW;
}
void CHFVPreviewView::set_pane_index(int index){set_pane(index);}

BEGIN_MESSAGE_MAP(CHFVPreviewView, CView)
	//{{AFX_MSG_MAP(CHFVPreviewView)
	ON_WM_PAINT()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CHFVPreviewView drawing

void CHFVPreviewView::OnDraw(CDC* pDC)
{
	CDocument* pDoc = GetDocument();
	// TODO: add draw code here
}

/////////////////////////////////////////////////////////////////////////////
// CHFVPreviewView diagnostics

#ifdef _DEBUG
void CHFVPreviewView::AssertValid() const
{
	CView::AssertValid();
}

void CHFVPreviewView::Dump(CDumpContext& dc) const
{
	CView::Dump(dc);
}
#endif //_DEBUG


void CHFVPreviewView::set_show( int onoff )
{
	m_show = onoff;
	update();
}

void CHFVPreviewView::set_file( list_lparam_struct *lparam )
{
	if(!lparam){m_show=0;memset(&m_lparam,0,sizeof(m_lparam));update();return;}
	memcpy( &m_lparam, lparam, sizeof(m_lparam) );
	m_show = 1;
	update();
}

void CHFVPreviewView::update()
{
	Invalidate();
}

/////////////////////////////////////////////////////////////////////////////
// CHFVPreviewView message handlers

void CHFVPreviewView::show_pc_info( CDC *pdc, int *px, int *py, CRect *prect )
{
	CString text = "PC info.";
	CRect r;
	
	r = CRect( *px, *py, prect->BottomRight().x, *py + m_lineheight );
	pdc->DrawText( text, text.GetLength(), &r, 0 );
	*py += m_lineheight;
}

void CHFVPreviewView::show_mac_info( CDC *pdc, int *px, int *py, CRect *prect )
{
	CString text = "Mac info.";
	CRect r;
	
	r = CRect( *px, *py, prect->BottomRight().x, *py + m_lineheight );
	pdc->DrawText( text, text.GetLength(), &r, 0 );
	*py += m_lineheight;
}

void CHFVPreviewView::show_contents( CDC *pdc, int *px, int *py, CRect *prect )
{
	CString text = "Contents.";
	CRect r;
	
	r = CRect( *px, *py, prect->BottomRight().x, *py + m_lineheight );
	pdc->DrawText( text, text.GetLength(), &r, 0 );
	*py += m_lineheight;
}

const int MINSHOWSIZE = 10;

void CHFVPreviewView::OnPaint() 
{
	int x, y, height;
	CRect cr;

	if(!m_show) return;

	GetClientRect( &cr );
	height = cr.Height();
	if( height < MINSHOWSIZE ) return;

	x = 5;
	y = 5;
	CPaintDC dc(this);

	show_pc_info( &dc, &x, &y, &cr );
	if(y < height) {
		show_mac_info( &dc, &x, &y, &cr );
		if(y < height) {
			show_contents( &dc, &x, &y, &cr );
		}
	}
	// Do not call CView::OnPaint() for painting messages
}
