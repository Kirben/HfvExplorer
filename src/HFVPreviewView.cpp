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
#include "HFVPreviewView.h"
#include "BrowserSession.h"
#include "HFVExplorerListView.h"
#include "resource.h"

namespace {
constexpr UINT_PTR PreviewTimer = 1;
constexpr UINT HidePreview = 1;
constexpr int Margin = 10, SmallWidth = 510, Line = 18;
constexpr int PreviewTextId = 2;

std::wstring FinderCode(const uint8_t* code) {
    if (!code[0] && !code[1] && !code[2] && !code[3]) return L"\x2014";
    std::wstring result(4, L'?');
    MultiByteToWideChar(10000, 0, reinterpret_cast<const char*>(code), 4, result.data(), 4);
    for (auto& c : result) if (c < 0x20 || c == 0x7f) c = L'?';
    return result;
}

bool HasType(const MacBrowser::Entry& entry, const char* type) {
    return memcmp(entry.finder.data(), type, 4) == 0;
}

std::wstring SizeText(uint64_t bytes) {
    return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
}

void DrawLine(CDC& dc, const std::wstring& text, CRect area) {
    if (area.Width() > 0)
        DrawTextW(dc.GetSafeHdc(), text.c_str(), int(text.size()), &area,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
}

IMPLEMENT_DYNCREATE(CHFVPreviewView, CView)
BEGIN_MESSAGE_MAP(CHFVPreviewView, CView)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_TIMER()
    ON_CONTROL(BN_CLICKED, HidePreview, OnClosePane)
END_MESSAGE_MAP()

CHFVPreviewView::~CHFVPreviewView() {
    if (session_) session_->Detach(this);
}

int CHFVPreviewView::OnCreate(LPCREATESTRUCT create) {
    if (CView::OnCreate(create) == -1) return -1;
    ModifyStyle(0,WS_CLIPCHILDREN);
    close_ = CreateWindowExW(0,L"BUTTON",L"Hide",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                            0,0,0,0,m_hWnd,reinterpret_cast<HMENU>(UINT_PTR(HidePreview)),nullptr,nullptr);
    text_ = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_TABSTOP|WS_VSCROLL|
                           ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|ES_NOHIDESEL,
                           0,0,0,0,m_hWnd,reinterpret_cast<HMENU>(UINT_PTR(PreviewTextId)),nullptr,nullptr);
    if (!close_ || !text_) return -1;
    ApplyDpi(NativeDpi::Window(m_hWnd));
    return 0;
}

void CHFVPreviewView::BindBrowserSession(BrowserSession* session) {
    if (session_ && session_ != session) Clear();
    session_ = session;
}

void CHFVPreviewView::EnablePreview(bool enabled) {
    enabled_ = enabled;
    if (enabled) RequestSelection();
    else Clear();
}

void CHFVPreviewView::Clear() {
    selectionPending_ = false;
    request_.Cancel();
    drive_ = -1;
    if (!GetSafeHwnd()) return;
    KillTimer(PreviewTimer);
    name_.clear();
    details_.clear();
    SetText(L"Select a file to preview.");
}

void CHFVPreviewView::MediaChanged(DWORD mask) {
    if (drive_ >= 0 && (mask & (DWORD(1) << drive_))) {
        Clear();
        SetText(L"The selected media changed.");
    }
}

void CHFVPreviewView::RequestSelection() {
    if (!enabled_ || !GetSafeHwnd()) return;
    request_.Cancel();
    selectionPending_ = true;
    SetTimer(PreviewTimer, 40, nullptr);
}

void CHFVPreviewView::ReadSelection() {
    auto list = session_ ? session_->List() : nullptr;
    if (!list || !session_->ListingAvailable()) { Clear(); return; }
    auto& control = list->GetListCtrl();
    const auto row = control.GetSelectedCount() == 1 ?
        list->BrowserRowAt(control.GetNextItem(-1, LVNI_SELECTED)) : nullptr;
    if (!row || row->location.IsNone()) { Clear(); return; }
    const auto& location=row->location;
    drive_=-1;
    if(location.IsPhysical())drive_=location.Drive();
    else if(location.IsMounted())drive_=location.Volume()->CdDrive();
    else if(location.IsWindows()){
        const auto& path=location.WindowsPath();
        if(path.size()>1&&path[1]==L':'){
            const auto letter=towupper(path[0]);
            if(letter>=L'A'&&letter<=L'Z')drive_=letter-L'A';
        }
    }
    const auto& entry = row->entry;
    name_ = MacBrowser::Wide(entry.name);
    const bool mac = !row->location.IsWindows();
    const bool finderText = HasType(entry, "TEXT") || HasType(entry, "ttro");
    const bool pict = HasType(entry, "PICT");
    const bool hasFinder = mac || memcmp(entry.finder.data(), "\0\0\0\0\0\0\0\0", 8) != 0;
    details_.clear();
    details_.emplace_back(L"Kind", entry.directory ? L"Folder" : finderText ? L"Text document" : pict ? L"PICT image" : L"File");
    const auto date = [&](const wchar_t* label, uint64_t ticks) {
        const auto formatted = BrowserDirectory::DateText(ticks);
        if (!formatted.empty()) details_.emplace_back(label, formatted);
    };
    date(L"Created", row->created);
    date(L"Modified", row->modified);
    if (!entry.directory) {
        if (hasFinder) details_.emplace_back(L"Finder codes",
            FinderCode(entry.finder.data()) + L" / " + FinderCode(entry.finder.data() + 4));
        if (hasFinder || entry.forks[1].size) {
            details_.emplace_back(L"Data fork", SizeText(entry.forks[0].size));
            details_.emplace_back(L"Resource fork", SizeText(entry.forks[1].size));
        } else details_.emplace_back(L"Size", SizeText(entry.forks[0].size));
    }
    if (entry.directory) { SetText(L"Select a text file to preview its contents."); return; }
    if (!entry.readError.empty() || !row->capabilities.read) { SetText(L"Could not read this file."); return; }
    if (pict || HasType(entry, "APPL")) { SetText(L"Content preview unavailable for this file type."); return; }
    SetText(L"Loading text preview...");
    request_.Start(row->location, row->accessKey, mac, finderText);
}

void CHFVPreviewView::OnTimer(UINT_PTR timer) {
    if (timer != PreviewTimer) { CView::OnTimer(timer); return; }
    try {
        if (selectionPending_) {
            selectionPending_ = false;
            ReadSelection();
        }
        if (request_.Pending()) {
            BrowserPreview::Text text;
            switch (request_.Take(text)) {
            case BrowserPreview::Request::State::Pending: return;
            case BrowserPreview::Request::State::Ready:
                if (text.content.empty()) SetText(L"Empty data fork");
                else SetText(L"Text preview  \x00b7  " + text.encoding +
                             (text.truncated ? L"  \x00b7  First 16 KiB" : L""), text.content);
                break;
            case BrowserPreview::Request::State::Busy: SetText(L"Storage is busy."); break;
            case BrowserPreview::Request::State::Stale: SetText(L"The selected file changed."); break;
            case BrowserPreview::Request::State::Unavailable: SetText(L"Content preview unavailable for this file type."); break;
            case BrowserPreview::Request::State::Failed: SetText(L"Could not read the text preview."); break;
            }
        }
    } catch (const std::exception&) {
        request_.Cancel();
        SetText(L"Could not load the preview.");
    }
    KillTimer(PreviewTimer);
}

void CHFVPreviewView::SetText(const std::wstring& status, const std::wstring& text) {
    status_ = status;
    if (text_) {
        SetWindowTextW(text_, text.c_str());
        ::ShowWindow(text_, text.empty() ? SW_HIDE : SW_SHOWNA);
    }
    Layout();
    Invalidate();
}

int CHFVPreviewView::Pixels(int value) const {
    return NativeDpi::Scale(value, dpi_);
}

void CHFVPreviewView::ApplyDpi(UINT dpi) {
    dpi_ = dpi;
    CFont replacement;
    if (replacement.CreateFont(-Pixels(12), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI")) {
        SendMessageW(close_, WM_SETFONT, WPARAM(replacement.GetSafeHandle()), FALSE);
        SendMessageW(text_, WM_SETFONT, WPARAM(replacement.GetSafeHandle()), FALSE);
        font_.DeleteObject();
        font_.Attach(replacement.Detach());
    }
    Layout();
    Invalidate();
}

CRect CHFVPreviewView::TextRect() const {
    CRect bounds;
    GetClientRect(bounds);
    const bool narrow = bounds.Width() < Pixels(SmallWidth);
    const auto left = narrow ? Pixels(Margin) : bounds.Width()*43/100;
    const auto top = narrow ? Pixels(2*Margin + Line*(3 + int(details_.size()))) : Pixels(2*Margin + 2*Line);
    return CRect(left, top, (std::max<LONG>)(left,bounds.right-Pixels(Margin)), (std::max<LONG>)(top,bounds.bottom-Pixels(Margin)));
}

void CHFVPreviewView::Layout() {
    if (!close_ || !text_) return;
    CRect bounds;
    GetClientRect(bounds);
    ::MoveWindow(close_, (std::max<LONG>)(0,bounds.right-Pixels(Margin+48)), Pixels(5), Pixels(48), Pixels(23), TRUE);
    const auto text = TextRect();
    ::MoveWindow(text_, text.left, text.top, text.Width(), text.Height(), TRUE);
}

void CHFVPreviewView::OnSize(UINT type, int cx, int cy) {
    CView::OnSize(type, cx, cy);
    Layout();
    Invalidate();
}

void CHFVPreviewView::OnDraw(CDC* dc) {
    CRect bounds;
    GetClientRect(bounds);
    dc->FillSolidRect(bounds, GetSysColor(COLOR_WINDOW));
    dc->SetBkMode(TRANSPARENT);
    auto oldFont = SelectObject(dc->GetSafeHdc(), font_.GetSafeHandle());
    dc->SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
    DrawLine(*dc, name_.empty() ? L"Preview" : L"Preview  \x2014  " + name_,
             CRect(Pixels(Margin), Pixels(5), bounds.right-Pixels(2*Margin+48), Pixels(5+Line)));
    const auto text = TextRect();
    const auto right = bounds.Width() < Pixels(SmallWidth) ? bounds.right-Pixels(Margin) : text.left-Pixels(Margin);
    int y = Pixels(Margin + Line);
    for (const auto& detail : details_) {
        DrawLine(*dc, detail.first, CRect(Pixels(Margin),y,Pixels(Margin+88),y+Pixels(Line)));
        DrawLine(*dc, detail.second, CRect(Pixels(Margin+90),y,right,y+Pixels(Line)));
        y += Pixels(Line);
    }
    DrawLine(*dc, status_, CRect(text.left,text.top-Pixels(Line+2),text.right,text.top-Pixels(2)));
    SelectObject(dc->GetSafeHdc(),oldFont);
}

void CHFVPreviewView::OnClosePane() {
    if (session_ && session_->Owner()) session_->Owner()->SendMessage(WM_COMMAND, ID_VIEW_PREVIEW_PANE);
}
