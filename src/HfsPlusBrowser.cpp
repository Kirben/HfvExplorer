// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "HfsPlus.h"
#include "HfsPlusBrowser.h"
#include "StorageJobDialog.h"
#include "ImageSize.h"
#include <commdlg.h>
namespace {
#ifdef HFV_TESTING
bool hiddenHfsPlusTest = false;
#else
const bool hiddenHfsPlusTest = false;
#endif
std::u16string U16(const std::wstring &s) { return std::u16string(s.begin(), s.end()); }
std::wstring Wide(const std::u16string &s) { return std::wstring(s.begin(), s.end()); }
std::wstring WidePath(const char *path) {
    int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring s(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_ACP, 0, path, -1, &s[0], n);
    return s;
}
void Cell(HWND list, int row, int col, const std::wstring &text) {
    LVITEMW item = {};
    item.iItem = row;
    item.iSubItem = col;
    item.pszText = const_cast<wchar_t *>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
}
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
            HfsPlus::Normalize(U16(value));
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
    explicit FormatDialog(CWnd *p) : CDialog(IDD_HPLUS_FORMAT, p) {}
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd, IDC_HPLUS_NAME);
        SetDlgItemTextW(m_hWnd, IDC_HPLUS_NAME, L"Untitled");
        SetDlgItemText(IDC_HPLUS_SIZE, "128 MiB");
        CheckDlgButton(IDC_HPLUS_JOURNAL, BST_CHECKED);
        return TRUE;
    }
    void OnOK() override {
        try {
            name = HfsPlus::Normalize(U16(Text(m_hWnd, IDC_HPLUS_NAME)));
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
class Browser : public CDialog {
    std::unique_ptr<HfsPlus::Source> source_;
    std::unique_ptr<HfsPlus::Volume> volume_;
    CListCtrl list_;
    uint32_t folder_ = 2;
    bool broken_ = false;
#ifdef HFV_TESTING
    unsigned lastJobTicks_ = 0;
#endif
    std::vector<HfsPlus::Entry> entries_;
    bool Run(std::function<void(Storage::Job &)> action, const char *title = "HFS+ storage",
             bool transfer = false) {
        struct Owner {
            CWinApp *app = AfxGetApp();
            CWnd *old = app->m_pMainWnd;
            explicit Owner(CWnd *window) { app->m_pMainWnd = window; }
            ~Owner() { app->m_pMainWnd = old; }
        } owner(this);
        auto work = [&](Storage::Job &job) {
            action(job);
            return Storage::Result{};
        };
        auto result = transfer ? ShowStorageJob(work, title, hiddenHfsPlusTest, true)
                               : ShowStorageWaitJob(work, hiddenHfsPlusTest);
#ifdef HFV_TESTING
        lastJobTicks_ = storageUiTest.timerTicks;
#endif
        if (result.code) {
            if (!hiddenHfsPlusTest)
                AfxMessageBox(result.message.c_str(), MB_OK | MB_ICONERROR);
            if (volume_ && !volume_->Writable() && source_->Writable()) {
                // Discard an aborted in-memory transaction; replay a durable
                // journal if the failed operation had reached its commit point.
                auto recovered = ShowStorageWaitJob(
                    [&](Storage::Job &) {
                        try {
                            volume_.reset(new HfsPlus::Volume(*source_, true));
                        } catch (...) {
                            volume_.reset(new HfsPlus::Volume(*source_, false));
                        }
                        return Storage::Result{};
                    },
                    hiddenHfsPlusTest);
                if (recovered.code) {
                    broken_ = true;
                    for (int id : {IDC_HPLUS_DATA, IDC_HPLUS_RESOURCE, IDC_HPLUS_UP})
                        GetDlgItem(id)->EnableWindow(FALSE);
                }
            }
            if (volume_ && !volume_->Writable()) {
                GetDlgItem(IDC_HPLUS_IMPORT)->EnableWindow(FALSE);
                GetDlgItem(IDC_HPLUS_IMPORT_RESOURCE)->EnableWindow(FALSE);
                GetDlgItem(IDC_HPLUS_MKDIR)->EnableWindow(FALSE);
                GetDlgItem(IDC_HPLUS_RENAME)->EnableWindow(FALSE);
                GetDlgItem(IDC_HPLUS_DELETE)->EnableWindow(FALSE);
            }
            return false;
        }
        return true;
    }
    void Reload() {
        if (broken_)
            return;
        if (!Run([&](Storage::Job &) { entries_ = volume_->List(folder_); }))
            return;
        list_.DeleteAllItems();
        for (size_t i = 0; i < entries_.size(); ++i) {
            auto &e = entries_[i];
            auto name = Wide(e.name);
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = int(i);
            item.pszText = &name[0];
            SendMessageW(list_.m_hWnd, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            Cell(list_.m_hWnd, int(i), 1, e.directory ? L"Folder" : L"File");
            Cell(list_.m_hWnd, int(i), 2, e.directory ? L"" : std::to_wstring(e.forks[0].size));
            Cell(list_.m_hWnd, int(i), 3, e.directory ? L"" : std::to_wstring(e.forks[1].size));
        }
        auto title = Wide(volume_->Name()) + L" - HFS+" + (volume_->Journaled() ? L" (journaled)" : L"") +
                     (volume_->Writable() ? L"" : L" (read-only)");
        SetWindowTextW(m_hWnd, title.c_str());
        auto status = Wide(volume_->Stat(folder_).name) + L"  |  " + std::to_wstring(entries_.size()) +
                      L" items  |  " + std::to_wstring(volume_->FreeBytes()) + L" bytes free";
        SetDlgItemTextW(m_hWnd, IDC_HPLUS_STATUS, status.c_str());
        GetDlgItem(IDC_HPLUS_UP)->EnableWindow(folder_ != 2);
    }
    int Selected() { return list_.GetNextItem(-1, LVNI_SELECTED); }
    void Export(unsigned fork) {
        int i = Selected();
        if (i < 0 || entries_[i].directory)
            return;
        auto entry = entries_[i];
        std::wstring path = Wide(entry.name) + (fork ? L".rsrc" : L"");
        for (auto &c : path)
            if (wcschr(L"<>:\"/\\|?*", c) || c < 32)
                c = L'_';
        if (!FileDialog(m_hWnd, path, true, L"All files\0*.*\0\0", nullptr))
            return;
        Run(
            [&](Storage::Job &job) {                          // create ordinary output, including empty forks
                volume_->Read(entry.id, fork, 0, nullptr, 0); // validate compressed/hard-link semantics
                HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                    throw HfsPlus::Error("Cannot create output file; choose a new filename.");
                try {
                    std::vector<uint8_t> data(1024 * 1024);
                    for (uint64_t offset = 0; offset < entry.forks[fork].size;) {
                        size_t n = size_t((std::min)(uint64_t(data.size()), entry.forks[fork].size - offset));
                        if (volume_->Read(entry.id, fork, offset, data.data(), n) != n)
                            throw HfsPlus::Error("Short HFS+ fork read");
                        DWORD done;
                        if (!WriteFile(file, data.data(), DWORD(n), &done, nullptr) || done != n)
                            throw HfsPlus::Error("Output write failed");
                        offset += n;
                        Storage::Progress progress;
                        progress.phase = "Exporting";
                        progress.scanning = false;
                        progress.byteTransfer = true;
                        progress.bytes = progress.fileBytes = offset;
                        progress.totalBytes = progress.fileTotal = entry.forks[fork].size;
                        job.progress(progress);
                    }
                    if (!FlushFileBuffers(file))
                        throw HfsPlus::Error("Output flush failed");
                    CloseHandle(file);
                } catch (...) {
                    CloseHandle(file);
                    DeleteFileW(path.c_str());
                    throw;
                }
            },
            fork ? "Export resource fork" : "Export data fork", true);
    }

  public:
#ifdef HFV_TESTING
    bool TestFailedImportRecovery() {
        bool result = Run([&](Storage::Job &) {
            volume_->Put(2, u"Failed import", 100, 0, [](unsigned, uint64_t, void *, size_t) {
                Sleep(250);
                throw HfsPlus::Error("Injected read failure");
            });
        });
        if (result || broken_ || !volume_->Writable() || !lastJobTicks_)
            return false;
        for (auto &entry : volume_->List())
            if (entry.name == u"Failed import")
                return false;
        return true;
    }
#endif
    Browser(std::unique_ptr<HfsPlus::Source> source, std::unique_ptr<HfsPlus::Volume> volume, CWnd *parent)
        : CDialog(IDD_HPLUS_BROWSER, parent), source_(std::move(source)), volume_(std::move(volume)) {}
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog();
        list_.SubclassDlgItem(IDC_HPLUS_LIST, this);
        list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        list_.SendMessage(CCM_SETUNICODEFORMAT, TRUE);
        list_.InsertColumn(0, "Name", LVCFMT_LEFT, 330);
        list_.InsertColumn(1, "Kind", LVCFMT_LEFT, 65);
        list_.InsertColumn(2, "Data bytes", LVCFMT_RIGHT, 115);
        list_.InsertColumn(3, "Resource bytes", LVCFMT_RIGHT, 115);
        for (int id : {IDC_HPLUS_IMPORT, IDC_HPLUS_MKDIR, IDC_HPLUS_RENAME, IDC_HPLUS_DELETE,
                       IDC_HPLUS_IMPORT_RESOURCE})
            GetDlgItem(id)->EnableWindow(volume_->Writable());
        Reload();
        return TRUE;
    }
    BOOL OnNotify(WPARAM w, LPARAM l, LRESULT *result) override {
        auto n = reinterpret_cast<NMHDR *>(l);
        if (n->idFrom == IDC_HPLUS_LIST && n->code == NM_DBLCLK) {
            int i = Selected();
            if (i >= 0 && entries_[i].directory) {
                folder_ = entries_[i].id;
                Reload();
            }
            *result = 0;
            return TRUE;
        }
        return CDialog::OnNotify(w, l, result);
    }
    BOOL OnCommand(WPARAM w, LPARAM l) override {
        unsigned command = LOWORD(w);
        if (broken_ && command >= IDC_HPLUS_LIST && command <= IDC_HPLUS_IMPORT_RESOURCE)
            return TRUE;
        if (command == IDC_HPLUS_UP) {
            if (folder_ != 2) {
                folder_ = volume_->Stat(folder_).parent;
                Reload();
            }
            return TRUE;
        }
        if (command == IDC_HPLUS_DATA || command == IDC_HPLUS_RESOURCE) {
            Export(command == IDC_HPLUS_RESOURCE);
            return TRUE;
        }
        if (command == IDC_HPLUS_MKDIR) {
            NameDialog name(this);
            name.value = L"New folder";
            if (name.DoModal() == IDOK) {
                Run([&](Storage::Job &) { volume_->MakeDirectory(folder_, U16(name.value)); });
                Reload();
            }
            return TRUE;
        }
        if (command == IDC_HPLUS_RENAME) {
            int i = Selected();
            if (i < 0)
                return TRUE;
            auto e = entries_[i];
            NameDialog name(this);
            name.value = Wide(e.name);
            if (name.DoModal() == IDOK) {
                Run([&](Storage::Job &) { volume_->Rename(e.id, folder_, U16(name.value)); });
                Reload();
            }
            return TRUE;
        }
        if (command == IDC_HPLUS_DELETE) {
            int i = Selected();
            if (i < 0)
                return TRUE;
            auto e = entries_[i];
            if (MessageBoxW(m_hWnd, (L"Delete " + Wide(e.name) + L" from this image?").c_str(),
                            L"Delete HFS+ entry", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                Run([&](Storage::Job &) { volume_->Remove(e.id); });
                Reload();
            }
            return TRUE;
        }
        if (command == IDC_HPLUS_IMPORT_RESOURCE) {
            int i = Selected();
            if (i < 0 || entries_[i].directory)
                return TRUE;
            auto entry = entries_[i];
            std::wstring path;
            if (!FileDialog(m_hWnd, path, false, L"Resource fork files\0*.*\0\0", nullptr))
                return TRUE;
            if (MessageBoxW(m_hWnd,
                            L"Replace this file's resource fork? Its data fork and Finder information will "
                            L"be preserved.",
                            L"Import resource fork", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            Run(
                [&](Storage::Job &job) {
                    auto in = HfsPlus::OpenImage(path.c_str(), false);
                    uint64_t size = in->Size();
                    volume_->Put(
                        entry.parent, entry.name, entry.forks[0].size, size,
                        [&](unsigned fork, uint64_t offset, void *bytes, size_t n) {
                            if (fork)
                                in->Read(offset, bytes, n);
                            else if (volume_->Read(entry.id, 0, offset, bytes, n) != n)
                                throw HfsPlus::Error("Short source data-fork read");
                            Storage::Progress p;
                            p.phase = fork ? "Importing resource fork" : "Preserving data fork";
                            p.scanning = false;
                            p.byteTransfer = true;
                            p.bytes = p.fileBytes = (fork ? entry.forks[0].size : 0) + offset + n;
                            p.totalBytes = p.fileTotal = entry.forks[0].size + size;
                            job.progress(p);
                        },
                        &entry.finder, true);
                },
                "Import HFS+ resource fork", true);
            Reload();
            return TRUE;
        }
        if (command == IDC_HPLUS_IMPORT) {
            std::wstring path;
            if (!FileDialog(m_hWnd, path, false, L"All files\0*.*\0\0", nullptr))
                return TRUE;
            NameDialog name(this);
            auto slash = path.find_last_of(L"\\/");
            name.value = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            if (name.DoModal() != IDOK)
                return TRUE;
            bool overwrite = false;
            for (auto &e : entries_)
                if (!HfsPlus::CompareNames(e.name, HfsPlus::Normalize(U16(name.value)))) {
                    if (MessageBoxW(m_hWnd, L"Replace the existing file and its resource fork?",
                                    L"HFS+ overwrite", MB_YESNO | MB_ICONQUESTION) != IDYES)
                        return TRUE;
                    overwrite = true;
                    break;
                }
            Run(
                [&](Storage::Job &job) {
                    auto in = HfsPlus::OpenImage(path.c_str(), false);
                    uint64_t size = in->Size();
                    volume_->Put(
                        folder_, U16(name.value), size, 0,
                        [&](unsigned, uint64_t offset, void *bytes, size_t n) {
                            in->Read(offset, bytes, n);
                            Storage::Progress p;
                            p.phase = "Importing";
                            p.scanning = false;
                            p.byteTransfer = true;
                            p.bytes = p.fileBytes = offset + n;
                            p.totalBytes = p.fileTotal = size;
                            job.progress(p);
                        },
                        nullptr, overwrite);
                },
                "Import file into HFS+", true);
            Reload();
            return TRUE;
        }
        return CDialog::OnCommand(w, l);
    }
};
} // namespace
int TryOpenHfsPlusBrowser(const char *path, CWnd *parent) {
    if (!path || !*path)
        return 0;
    auto wide = WidePath(path);
    std::unique_ptr<HfsPlus::Source> source;
    std::unique_ptr<HfsPlus::Volume> volume;
    bool recognized = false;
    auto result = ShowStorageWaitJob([&](Storage::Job &) {
        source = HfsPlus::OpenImage(wide.c_str(), false);
        recognized = HfsPlus::Volume::Recognizes(*source);
        if (!recognized)
            return Storage::Result{};
        source.reset();
        bool writable = true;
        try {
            source = HfsPlus::OpenImage(wide.c_str(), true);
        } catch (...) {
            writable = false;
            source = HfsPlus::OpenImage(wide.c_str(), false);
        }
        try {
            volume.reset(new HfsPlus::Volume(*source, writable));
        } catch (...) {
            if (!writable)
                throw;
            source.reset();
            source = HfsPlus::OpenImage(wide.c_str(), false);
            volume.reset(new HfsPlus::Volume(*source, false));
        }
        return Storage::Result{};
    });
    if (!recognized)
        return 0;
    if (result.code) {
        AfxMessageBox(("Cannot open HFS+ image: " + result.message).c_str());
        return -1;
    }
    Browser browser(std::move(source), std::move(volume), parent);
    return browser.DoModal() == -1 ? -1 : 1;
}
void FormatHfsPlusImage(CWnd *parent) {
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
            HfsPlus::Volume::Format(*source, options.name, options.journaled);
            return Storage::Result{};
        },
        "Format HFS+ image");
    if (result.code) {
        AfxMessageBox(result.message.c_str());
        return;
    }
    std::unique_ptr<HfsPlus::Source> source;
    std::unique_ptr<HfsPlus::Volume> volume;
    result = ShowStorageWaitJob([&](Storage::Job &) {
        source = HfsPlus::OpenImage(path.c_str(), true);
        volume.reset(new HfsPlus::Volume(*source, true));
        return Storage::Result{};
    });
    if (result.code) {
        AfxMessageBox(result.message.c_str());
        return;
    }
    Browser browser(std::move(source), std::move(volume), parent);
    browser.DoModal();
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
        hiddenHfsPlusTest = true;
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
        format.DestroyWindow();
        std::wstring path = WidePath((std::string(directory) + "\\ui.dsk").c_str());
        {
            auto source = HfsPlus::CreateImage(path.c_str(), 16 * 1024 * 1024);
            HfsPlus::Volume::Format(*source, u"UI tests");
            auto volume = std::unique_ptr<HfsPlus::Volume>(new HfsPlus::Volume(*source, true));
            volume->MakeDirectory(2, u"\u65e5\u672c \u03b1");
            Browser browser(std::move(source), std::move(volume), &owner);
            require(browser.Create(IDD_HPLUS_BROWSER, &owner) != FALSE, "Browser creation");
            require(::SendDlgItemMessage(browser.m_hWnd, IDC_HPLUS_LIST, LVM_GETITEMCOUNT, 0, 0) == 3,
                    "Browser root listing");
            require(owner.IsWindowEnabled() != FALSE && browser.IsWindowEnabled() != FALSE,
                    "Owner was not restored after listing");
            require(AfxGetApp()->m_pMainWnd == &owner, "Browser leaked job owner override");
            require(browser.TestFailedImportRecovery(), "Slow failed import did not recover responsively");
            require(owner.IsWindowEnabled() && browser.IsWindowEnabled() && AfxGetApp()->m_pMainWnd == &owner,
                    "Owner was not restored after failed import and recovery");
            browser.DestroyWindow();
        }
        {
            auto source = HfsPlus::OpenImage(path.c_str(), false);
            auto volume = std::unique_ptr<HfsPlus::Volume>(new HfsPlus::Volume(*source));
            Browser browser(std::move(source), std::move(volume), &owner);
            require(browser.Create(IDD_HPLUS_BROWSER, &owner) != FALSE, "Read-only browser");
            for (int id : {IDC_HPLUS_IMPORT, IDC_HPLUS_IMPORT_RESOURCE, IDC_HPLUS_MKDIR, IDC_HPLUS_RENAME,
                           IDC_HPLUS_DELETE})
                require(!browser.GetDlgItem(id)->IsWindowEnabled(), "Read-only write control enabled");
            browser.DestroyWindow();
        }
        fprintf(report, "PASS: hidden HFS+ browser and formatter, Unicode name editing, root listing, "
                        "journal default, read-only controls, serialized storage job and owner restoration. "
                        "No computer control or physical devices used.\n");
    } catch (const std::exception &e) {
        fprintf(report, "FAIL: %s\n", e.what());
        hiddenHfsPlusTest = false;
        AfxGetApp()->m_pMainWnd = nullptr;
        if (owner.m_hWnd)
            owner.DestroyWindow();
        fclose(report);
        return 1;
    }
    hiddenHfsPlusTest = false;
    AfxGetApp()->m_pMainWnd = nullptr;
    owner.DestroyWindow();
    fclose(report);
    return 0;
}
#endif
