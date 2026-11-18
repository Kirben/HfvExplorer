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

#pragma once
#include "BrowserPreview.h"

#define PANE_TREE 0
#define PANE_LIST 1

class BrowserSession;

class CHFVPreviewView : public CView {
    DECLARE_DYNCREATE(CHFVPreviewView)
    BrowserSession* session_ = nullptr;
    BrowserPreview::Request request_;
    HWND close_ = nullptr, text_ = nullptr;
    bool enabled_ = false, selectionPending_ = false;
    int drive_ = -1;
    std::wstring name_, status_;
    std::vector<std::pair<std::wstring,std::wstring>> details_;
    void ReadSelection();
    void SetText(const std::wstring& status, const std::wstring& text = {});
    CRect TextRect() const;
    void Layout();
protected:
    CHFVPreviewView() = default;
    ~CHFVPreviewView() override;
    void OnDraw(CDC*) override;
    afx_msg int OnCreate(LPCREATESTRUCT);
    afx_msg void OnSize(UINT,int,int);
    afx_msg void OnTimer(UINT_PTR);
    afx_msg void OnClosePane();
    DECLARE_MESSAGE_MAP()
public:
    BrowserSession* BrowserBinding() const { return session_; }
    void BindBrowserSession(BrowserSession*);
    bool Enabled() const { return enabled_; }
    void EnablePreview(bool);
    void RequestSelection();
    void Clear();
    void MediaChanged(DWORD mask);
};
