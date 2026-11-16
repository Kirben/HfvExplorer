// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MainMacBrowser.h"
#include "BrowserSession.h"
#include "resource.h"
#include "HfsPlus.h"
#include "HfsPlusDialogs.h"
#include "StorageJobDialog.h"
#include "ImageSize.h"
#include <commdlg.h>
namespace {
std::u16string U16(const std::wstring &s) { return std::u16string(s.begin(), s.end()); }
std::wstring Text(HWND parent, int id) {
    int n = GetWindowTextLengthW(GetDlgItem(parent, id));
    std::wstring s(size_t(n) + 1, L'\0');
    GetDlgItemTextW(parent, id, &s[0], n + 1);
    s.resize(n);
    return s;
}
void UnicodeEdit(HWND parent, int id) {
    HWND old = GetDlgItem(parent, id);
    if (IsWindowUnicode(old))
        return;
    RECT r;
    GetWindowRect(old, &r);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&r), 2);
    auto font = SendMessage(old, WM_GETFONT, 0, 0);
    auto previous = GetWindow(old, GW_HWNDPREV);
    DestroyWindow(old);
    HWND control =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        r.left, r.top, r.right - r.left, r.bottom - r.top, parent,
                        reinterpret_cast<HMENU>(INT_PTR(id)), AfxGetInstanceHandle(), nullptr);
    if (!control)
        throw HfsPlus::Error("Cannot create Unicode name control");
    SendMessage(control, WM_SETFONT, font, TRUE);
    SetWindowPos(control, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
bool FileDialog(HWND owner, std::wstring &path, bool save, const wchar_t *filter, const wchar_t *extension) {
    wchar_t buffer[32768] = {};
    wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? 0 : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)))
        return false;
    path = buffer;
    return true;
}
class NameDialog : public CDialog {
  public:
    std::wstring value;
    bool caseSensitive = false;
    explicit NameDialog(CWnd *p) : CDialog(IDD_HPLUS_NAME, p) {}
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd, IDC_HPLUS_NAME);
        SetDlgItemTextW(m_hWnd, IDC_HPLUS_NAME, value.c_str());
        SendDlgItemMessage(IDC_HPLUS_NAME, EM_SETLIMITTEXT, 255);
        return TRUE;
    }
    void OnOK() override {
        value = Text(m_hWnd, IDC_HPLUS_NAME);
        try {
            HfsPlus::Normalize(U16(value), caseSensitive);
            CDialog::OnOK();
        } catch (const std::exception &e) {
            AfxMessageBox(e.what());
        }
    }
};
class FormatDialog : public CDialog {
  public:
    std::u16string name;
    uint64_t size = 0;
    bool journaled = true;
    HfsPlus::Volume::FormatKind kind = HfsPlus::Volume::FormatKind::HfsPlus;
    explicit FormatDialog(CWnd *p) : CDialog(IDD_HPLUS_FORMAT, p) {}
    HfsPlus::Volume::FormatKind SelectedKind() {
        int selected = int(SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_GETCURSEL));
        if (selected < 0 || selected > 2)
            throw HfsPlus::Error("Choose a filesystem format.");
        return static_cast<HfsPlus::Volume::FormatKind>(selected);
    }
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd, IDC_HPLUS_NAME);
        SetDlgItemTextW(m_hWnd, IDC_HPLUS_NAME, L"Untitled");
        SetDlgItemText(IDC_HPLUS_SIZE, "128 MiB");
        CheckDlgButton(IDC_HPLUS_JOURNAL, BST_CHECKED);
        for (const char *label :
             {"HFS+ (case-insensitive)", "HFSX (case-sensitive)", "HFSX (case-insensitive)"})
            SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, 0);
        return TRUE;
    }
    void OnOK() override {
        try {
            kind = SelectedKind();
            name = HfsPlus::Normalize(U16(Text(m_hWnd, IDC_HPLUS_NAME)),
                                      kind == HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
            CString text;
            GetDlgItemText(IDC_HPLUS_SIZE, text);
            hfv_size_t parsed = 0;
            if (!ParseImageSize(text, &parsed, INT64_MAX))
                throw HfsPlus::Error("Enter an image size such as 128 MiB or 4 GiB.");
            size = parsed;
            if (size < 8 * 1024 * 1024 || size % 4096)
                throw HfsPlus::Error("Use at least 8 MiB and a multiple of 4096 bytes.");
            journaled = IsDlgButtonChecked(IDC_HPLUS_JOURNAL) == BST_CHECKED;
            CDialog::OnOK();
        } catch (const std::exception &e) {
            AfxMessageBox(e.what());
        }
    }
};
} // namespace
bool EditHfsPlusName(CWnd *parent, std::wstring &name, bool caseSensitive) {
 NameDialog dialog(parent);dialog.value=name;dialog.caseSensitive=caseSensitive;
 if(dialog.DoModal()!=IDOK)return false;name=dialog.value;return true;
}
void FormatHfsPlusImage(CWnd *parent,BrowserSession& session) {
    FormatDialog options(parent);
    if (options.DoModal() != IDOK)
        return;
    std::wstring path;
    if (!FileDialog(parent->m_hWnd, path, true, L"Mac disk images\0*.dsk;*.hfs;*.img\0All files\0*.*\0\0",
                    L"dsk"))
        return;
    auto result = ShowStorageJob(
        [&](Storage::Job &) {
            auto source = HfsPlus::CreateImage(path.c_str(), options.size);
            HfsPlus::Volume::Format(*source, options.name, options.journaled, options.kind);
            return Storage::Result{};
        },
        "Format Mac image",false,false,false,parent);
    if (result.code) {
        AfxMessageBox(result.message.c_str());
        return;
    }
    session.MacBrowser().OpenImage(path);
}
#ifdef HFV_TESTING
int RunHfsPlusUiTest(const char *directory) {
    if (!CreateDirectoryA(directory, nullptr))
        return 2;
    std::string reportPath = std::string(directory) + "\\hfsplus-ui-report.txt";
    FILE *report = fopen(reportPath.c_str(), "w");
    if (!report)
        return 2;
    auto require = [](bool value, const char *text) {
        if (!value)
            throw HfsPlus::Error(text);
    };
    CWnd owner;
    try {
        require(owner.CreateEx(0, AfxRegisterWndClass(0), "Hidden HFS+ owner", WS_OVERLAPPED, 0, 0, 100, 100,
                               nullptr, 0) != FALSE,
                "Hidden owner creation");
        AfxGetApp()->m_pMainWnd = &owner;
        InitializeStorageUi();
        NameDialog name(&owner);
        name.value = L"\x65e5\x672c \x03b1";
        require(name.Create(IDD_HPLUS_NAME, &owner) != FALSE, "Name dialog creation");
        require(IsWindowUnicode(::GetDlgItem(name.m_hWnd, IDC_HPLUS_NAME)) != FALSE, "Name field is ANSI");
        require(Text(name.m_hWnd, IDC_HPLUS_NAME) == name.value, "Unicode name round trip");
        name.DestroyWindow();
        FormatDialog format(&owner);
        require(format.Create(IDD_HPLUS_FORMAT, &owner) != FALSE, "Format dialog creation");
        require(IsWindowUnicode(::GetDlgItem(format.m_hWnd, IDC_HPLUS_NAME)) &&
                    format.IsDlgButtonChecked(IDC_HPLUS_JOURNAL) == BST_CHECKED,
                "Unicode format defaults");
        require(format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_GETCOUNT) == 3,
                "HFSX format choices missing");
        for (int choice = 0; choice < 3; ++choice) {
            format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, choice);
            require(static_cast<int>(format.SelectedKind()) == choice, "Format choice not applied");
        }
        format.DestroyWindow();
        fprintf(report, "PASS: active Unicode name and formatter dialogs; all three format choices, "
                        "journal default and owner restoration. Main-browser regressions run separately. "
                        "No computer control or physical devices used.\n");
    } catch (const std::exception &e) {
        fprintf(report, "FAIL: %s\n", e.what());
        AfxGetApp()->m_pMainWnd = nullptr;
        if (owner.m_hWnd)
            owner.DestroyWindow();
        fclose(report);
        return 1;
    }
    AfxGetApp()->m_pMainWnd = nullptr;
    owner.DestroyWindow();
    fclose(report);
    return 0;
}
#endif
