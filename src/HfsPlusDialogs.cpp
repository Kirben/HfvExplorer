// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "MainMacBrowser.h"
#include "BrowserSession.h"
#include "resource.h"
#include "HfsPlus.h"
#include "HfsPlusDialogs.h"
#include "StorageJobDialog.h"
#include "ImageSize.h"
#include "MfsImage.h"
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
bool FileDialog(HWND owner, std::wstring &path, bool save, const wchar_t *filter, const wchar_t *extension,const wchar_t* title=nullptr) {
    wchar_t buffer[32768] = {};
    wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = owner;ofn.lpstrTitle=title;
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
    bool journaled = true,mfs=false,dc42=false,expanded=false,external=false;
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
             {"HFS+ (case-insensitive)", "HFSX (case-sensitive)", "HFSX (case-insensitive)", "MFS (raw image)", "MFS (Disk Copy 4.2)"})
            SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, 0);
        return TRUE;
    }
    BOOL OnCommand(WPARAM wp,LPARAM lp)override{
        if(LOWORD(wp)==IDC_HPLUS_FORMAT_KIND&&HIWORD(wp)==CBN_SELCHANGE){int selected=int(SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_GETCURSEL));bool nowMfs=selected>=3;
            GetDlgItem(IDC_HPLUS_JOURNAL)->EnableWindow(!nowMfs);GetDlgItem(IDC_HPLUS_EXPANDED)->EnableWindow(!nowMfs);GetDlgItem(IDC_HPLUS_EXTERNAL)->EnableWindow(!nowMfs);if(nowMfs&&!mfs)SetDlgItemText(IDC_HPLUS_SIZE,"400 KiB");if(!nowMfs&&mfs)SetDlgItemText(IDC_HPLUS_SIZE,"128 MiB");mfs=nowMfs;return TRUE;}
        if(LOWORD(wp)==IDC_HPLUS_EXTERNAL&&IsDlgButtonChecked(IDC_HPLUS_EXTERNAL)==BST_CHECKED)CheckDlgButton(IDC_HPLUS_JOURNAL,BST_CHECKED);
        return CDialog::OnCommand(wp,lp);
    }
    void OnOK() override {
        try {
            int selected=int(SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_GETCURSEL));mfs=selected>=3;dc42=selected==4;
            if(mfs){std::string encoded,error;auto text=Text(m_hWnd,IDC_HPLUS_NAME);if(!MfsImage::EncodeName(text,encoded,error,27))throw HfsPlus::Error(error);name=U16(text);}
            else {kind = SelectedKind();
            name = HfsPlus::Normalize(U16(Text(m_hWnd, IDC_HPLUS_NAME)),
                                      kind == HfsPlus::Volume::FormatKind::HfsXCaseSensitive);}
            CString text;
            GetDlgItemText(IDC_HPLUS_SIZE, text);
            hfv_size_t parsed = 0;
            if (!ParseImageSize(text, &parsed, INT64_MAX))
                throw HfsPlus::Error("Enter an image size such as 128 MiB or 4 GiB.");
            size = parsed;
            if(mfs){if(size<128*1024||size>20*1024*1024||size%512)throw HfsPlus::Error("MFS requires 128 KiB-20 MiB and a 512-byte multiple.");if(dc42&&size!=400*1024&&size!=800*1024&&size!=720*1024&&size!=1440*1024)throw HfsPlus::Error("Disk Copy requires 400, 800, 720 or 1440 KiB.");}
            else if (size < 8 * 1024 * 1024 || size % 4096)throw HfsPlus::Error("Use at least 8 MiB and a multiple of 4096 bytes.");
            journaled = !mfs&&IsDlgButtonChecked(IDC_HPLUS_JOURNAL) == BST_CHECKED;expanded=!mfs&&IsDlgButtonChecked(IDC_HPLUS_EXPANDED)==BST_CHECKED;external=!mfs&&IsDlgButtonChecked(IDC_HPLUS_EXTERNAL)==BST_CHECKED;if(external&&!journaled)throw HfsPlus::Error("Enable journaling to use an external journal");
            CDialog::OnOK();
        } catch (const std::exception &e) {
            AfxMessageBox(e.what());
        }
    }
};
} // namespace
bool SelectHfsPlusExternalJournal(CWnd* owner,std::wstring& path,const std::string& prompt){auto title=std::wstring(prompt.begin(),prompt.end());return FileDialog(owner->m_hWnd,path,false,L"External journal disk images\0*.img;*.dsk\0All files\0*.*\0\0",L"img",title.c_str());}
void HfvUnicodeNameEdit(HWND parent,int id){UnicodeEdit(parent,id);}
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
    std::wstring externalPath;
    if(options.external&&!FileDialog(parent->m_hWnd,externalPath,true,L"External journal disk image\0*.img\0All files\0*.*\0\0",L"img"))return;
    auto access=CommandStorage::ImageWrite(path);if(options.external){auto other=CommandStorage::ImageWrite(externalPath);access.insert(access.end(),other.begin(),other.end());}
    std::vector<std::string> changed{VolumeCoord::ImageKey(path),VolumeCoord::PathKey(path)};if(options.external){changed.push_back(VolumeCoord::ImageKey(externalPath));changed.push_back(VolumeCoord::PathKey(externalPath));}
    auto result = ShowStorageJob(
        CommandStorage::Guard(access,[&](Storage::Job &) {
            bool homeCreated=false,journalCreated=false,complete=false;
            struct Cleanup{std::wstring home,journal;bool& h;bool& j;bool& complete;~Cleanup(){if(!complete){if(h)DeleteFileW(home.c_str());if(j)DeleteFileW(journal.c_str());}}}cleanup{path,externalPath,homeCreated,journalCreated,complete};
            auto bytes=options.size+(options.dc42?84+((options.size==400*1024||options.size==800*1024)?options.size/512*12:0):0);
            auto source = HfsPlus::CreateImage(path.c_str(),bytes,options.external?externalPath.c_str():nullptr);homeCreated=true;
            if(options.mfs){
                struct WritableMfs:MfsSource{HfsPlus::Source& s;explicit WritableMfs(HfsPlus::Source& v):s(v){}uint64_t Length()const override{return s.Size();}bool Writable()const override{return s.Writable();}bool ReadAt(uint64_t at,void* p,size_t n)override{try{s.Read(at,p,n);return true;}catch(...){return false;}}bool WriteAt(uint64_t at,const void* p,size_t n)override{try{s.Write(at,p,n);return true;}catch(...){return false;}}bool Flush()override{try{s.Flush();return true;}catch(...){return false;}}}mfs(*source);
                std::string name,error;if(!MfsImage::EncodeName(MacBrowser::Wide(options.name),name,error,27)||!MfsImage::Format(mfs,name,options.size,options.dc42,error))throw HfsPlus::Error(error);
            }else {HfsPlus::Volume::Format(*source, options.name, options.journaled, options.kind, options.expanded);if(options.external){auto uuid=HfsPlus::CreateExternalJournalImage(externalPath.c_str());journalCreated=true;HfsPlus::Volume volume(*source,true);volume.UseExternalJournal(uuid);}}
            complete=true;return Storage::Result{};
        },changed),
        "Format Mac image",false,false,false,parent);
    if (result.code) {
        AfxMessageBox(result.message.c_str());
        return;
    }
    session.MacBrowser().OpenImage(path,externalPath);
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
        require(format.IsDlgButtonChecked(IDC_HPLUS_EXPANDED)==BST_UNCHECKED&&format.IsDlgButtonChecked(IDC_HPLUS_EXTERNAL)==BST_UNCHECKED,"Advanced format defaults");
        format.CheckDlgButton(IDC_HPLUS_JOURNAL,BST_UNCHECKED);format.CheckDlgButton(IDC_HPLUS_EXTERNAL,BST_CHECKED);format.SendMessage(WM_COMMAND,IDC_HPLUS_EXTERNAL,0);require(format.IsDlgButtonChecked(IDC_HPLUS_JOURNAL)==BST_CHECKED,"External format did not require journaling");
        require(format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_GETCOUNT) == 5,
                "HFSX format choices missing");
        for (int choice = 0; choice < 3; ++choice) {
            format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, choice);
            require(static_cast<int>(format.SelectedKind()) == choice, "Format choice not applied");
        }
        for(int choice=3;choice<5;++choice){format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_SETCURSEL,choice);format.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_HPLUS_FORMAT_KIND,CBN_SELCHANGE),0);require(!format.GetDlgItem(IDC_HPLUS_EXTERNAL)->IsWindowEnabled()&&!format.GetDlgItem(IDC_HPLUS_EXPANDED)->IsWindowEnabled()&&!format.GetDlgItem(IDC_HPLUS_JOURNAL)->IsWindowEnabled()&&Text(format.m_hWnd,IDC_HPLUS_SIZE)==L"400 KiB","MFS formatter defaults/journal state");}
        format.DestroyWindow();
        fprintf(report, "PASS: active Unicode name and formatter dialogs; all HFS+/HFSX/MFS format choices, "
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
