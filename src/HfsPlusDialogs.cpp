// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "MainMacBrowser.h"
#include "BrowserSession.h"
#include "resource.h"
#include "HfsPlus.h"
#include "HfsPlusDialogs.h"
#include "DmgImage.h"
#include "DmgEncryption.h"
#include "StorageLocation.h"
#include <optional>
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
#include "ImageHandleTransfer.h"
#include "ImageSize.h"
#include "MfsImage.h"
#include <algorithm>
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
bool FileDialog(HWND owner, std::wstring &path, bool save, const wchar_t *filter, const wchar_t *extension,const wchar_t* title=nullptr,DWORD* fileType=nullptr) {
    wchar_t buffer[32768] = {};
    wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = owner;ofn.lpstrTitle=title;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? 0 : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)))
        return false;
    path = buffer;
    if(fileType)*fileType=ofn.nFilterIndex;
    return true;
}
class ImagePasswordDialog final:public CDialog {
    bool retry_;
    void Clear() {
        SetDlgItemTextW(m_hWnd,IDC_DMG_PASSWORD,L"");
        SendDlgItemMessage(IDC_DMG_PASSWORD,EM_EMPTYUNDOBUFFER,0,0);
    }
public:
    std::shared_ptr<const Dmg::Password> password;
    ImagePasswordDialog(CWnd* parent,bool retry):CDialog(IDD_DMG_PASSWORD,parent),retry_(retry){}
    BOOL OnInitDialog()override{
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd,IDC_DMG_PASSWORD);
        SendDlgItemMessage(IDC_DMG_PASSWORD,EM_SETLIMITTEXT,1023);
        if(retry_)SetDlgItemTextW(m_hWnd,IDC_DMG_PROMPT,L"The password did not unlock this image. Try again.");
        GetDlgItem(IDC_DMG_PASSWORD)->SetFocus();
        return FALSE;
    }
    void OnOK()override{
        struct Text {
            wchar_t value[1024]{};
            ~Text(){SecureZeroMemory(value,sizeof(value));}
        } text;
        const auto size=GetDlgItemTextW(m_hWnd,IDC_DMG_PASSWORD,text.value,int(std::size(text.value)));
        try{password=std::make_shared<Dmg::Password>(std::wstring_view(text.value,size));}
        catch(const std::exception& error){Clear();MessageBox(error.what(),"Unlock disk image",MB_ICONERROR);return;}
        Clear();
        CDialog::OnOK();
    }
    void OnCancel()override{Clear();CDialog::OnCancel();}
};
class NameDialog : public CDialog {
  public:
    std::wstring value;
    bool caseSensitive = false;
    bool windows = false;
    explicit NameDialog(CWnd *p) : CDialog(IDD_HPLUS_NAME, p) {}
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd, IDC_HPLUS_NAME);
        if(windows)SetWindowText("Rename");
        SetDlgItemTextW(m_hWnd, IDC_HPLUS_NAME, value.c_str());
        SendDlgItemMessage(IDC_HPLUS_NAME, EM_SETLIMITTEXT, 255);
        return TRUE;
    }
    void OnOK() override {
        value = Text(m_hWnd, IDC_HPLUS_NAME);
        try {
            if(windows){
                if(value.empty()||value==L"."||value==L".."||value.back()==L'.'||value.back()==L' '||
                   value.find_first_of(L"<>:\"/\\|?*")!=value.npos||
                   std::any_of(value.begin(),value.end(),[](wchar_t c){return c<32;}))
                    throw HfsPlus::Error("Enter a valid Windows filename.");
            }else HfsPlus::Normalize(U16(value), caseSensitive);
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
bool EditWindowsName(CWnd *parent, std::wstring &name) {
 NameDialog dialog(parent);dialog.value=name;dialog.windows=true;
 if(dialog.DoModal()!=IDOK)return false;name=dialog.value;return true;
}
bool EditHfsPlusName(CWnd *parent, std::wstring &name, bool caseSensitive) {
 NameDialog dialog(parent);dialog.value=name;dialog.caseSensitive=caseSensitive;
 if(dialog.DoModal()!=IDOK)return false;name=dialog.value;return true;
}
namespace {
class FormatMfsSource final : public MfsSource {
    HfsPlus::Source& image_;
public:
    explicit FormatMfsSource(HfsPlus::Source& image):image_(image){}
    uint64_t Length()const override{return image_.Size();}
    bool Writable()const override{return image_.Writable();}
    bool ReadAt(uint64_t at,void* p,size_t n)override{try{image_.Read(at,p,n);return true;}catch(...){return false;}}
    bool WriteAt(uint64_t at,const void* p,size_t n)override{try{image_.Write(at,p,n);return true;}catch(...){return false;}}
    bool Flush()override{try{image_.Flush();return true;}catch(...){return false;}}
};
}

std::function<Storage::Result(Storage::Job&)> MakeMacImageFormatJob(MacImageFormatRequest request)
{
    return [request=std::move(request)](Storage::Job& job){
        const bool external=!request.externalJournal.empty();
        if(external&&(request.mfs||!request.journaled))
            return Storage::Result{1,ERROR_INVALID_PARAMETER,"An external journal requires a journaled HFS+ or HFSX image."};
        if(request.mfs ? (request.size<128*1024||request.size>20*1024*1024||request.size%512)
                       : (request.size<8*1024*1024||request.size%4096||request.size>INT64_MAX))
            return Storage::Result{1,ERROR_INVALID_PARAMETER,"Invalid image size for the selected format."};
        auto access=CommandStorage::ImageWrite(request.path);
        if(external){
            const auto journal=CommandStorage::ImageWrite(request.externalJournal);
            if(journal.front().key==access.front().key||journal.back().key==access.back().key)
                return Storage::Result{1,ERROR_INVALID_PARAMETER,"Choose separate image and journal paths."};
            access.insert(access.end(),journal.begin(),journal.end());
        }
        std::vector<std::string> changed;
        for(const auto& item:access)changed.push_back(item.key);
        return CommandStorage::Guard(std::move(access),[&](Storage::Job&){
            constexpr uint64_t journalSize=64ULL*1024*1024;
            std::unique_ptr<HfsPlus::NewImage> home,journal;
            try{
                const auto tags=request.dc42&&(request.size==400*1024||request.size==800*1024)?request.size/512*12:0;
                const auto bytes=request.size+(request.dc42?84+tags:0);
                home=HfsPlus::CreateFormatOutput(request.path.c_str(),bytes,external?request.externalJournal.c_str():nullptr);
                if(request.mfs){
                    FormatMfsSource mfs(*home);
                    std::string name,error;
                    if(!MfsImage::EncodeName(MacBrowser::Wide(request.name),name,error,27)||
                       !MfsImage::Format(mfs,name,request.size,request.dc42,error))
                        throw HfsPlus::Error(error);
                }else{
                    HfsPlus::Volume::Format(*home,request.name,request.journaled,request.kind,request.expanded);
                    if(external){
                        journal=HfsPlus::CreateFormatOutput(request.externalJournal.c_str(),journalSize);
                        const auto uuid=HfsPlus::InitializeExternalJournalImage(*journal);
                        const std::vector<ImageHandleTransfer::File> files{
                            {request.externalJournal,HANDLE(journal->WorkerFiles().front().second),true}};
                        ImageHandleTransfer::Scope binding(files);
                        HfsPlus::Volume volume(*home,true);
                        volume.UseExternalJournal(uuid);
                    }
                }
                home->Keep();
                if(journal)journal->Keep();
                return Storage::Result{};
            }catch(...){
                const bool homeRemoved=!home||home->Discard();
                home.reset(); // Release any duplicate of the journal handle.
                const bool journalRemoved=!journal||journal->Discard();
                if(!homeRemoved||!journalRemoved)
                    throw HfsPlus::Error("Formatting failed and Windows could not remove a partial output. Inspect the image and journal paths.");
                throw;
            }
        },std::move(changed))(job);
    };
}

bool StartMacImageFormat(BrowserSession& session,MacImageFormatRequest request)
{
    auto complete=session.WhileOpen([path=request.path,journal=request.externalJournal]
                                  (BrowserSession& origin,const Storage::Result& result){
        if(!result.code)origin.MacBrowser().OpenImage(path,journal);
    });
    return BackgroundCopies::StartStorageResult(MakeMacImageFormatJob(std::move(request)),session.Owner(),
        "Formatting Mac image","Format image",std::move(complete));
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
    StartMacImageFormat(session,{std::move(path),std::move(externalPath),std::move(options.name),options.size,
                                options.kind,options.journaled,options.mfs,options.dc42,options.expanded});
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
        const auto folder=VolumeCoord::Wide(directory);
        auto run=[&](MacImageFormatRequest request){
            auto job=Storage::worker().submit(MakeMacImageFormatJob(std::move(request)));
            job->wait();
            return job->result();
        };
        using Kind=HfsPlus::Volume::FormatKind;
        for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
            const auto output=folder+L"\\formatted-"+std::to_wstring(int(kind))+L".dsk";
            require(!run({output,{},u"Background",8*1024*1024,kind}).code,"format HFS+ or HFSX image");
            auto image=HfsPlus::OpenImage(output.c_str(),false);
            HfsPlus::Volume volume(*image,false);
            require(volume.IsHfsX()==(kind!=Kind::HfsPlus)&&
                    volume.CaseSensitive()==(kind==Kind::HfsXCaseSensitive),"formatted case mode changed");
        }
        for(const bool dc42:{false,true}){
            const auto output=folder+(dc42?L"\\formatted.dc42":L"\\formatted-mfs.dsk");
            require(!run({output,{},u"MFS Background",400*1024,Kind::HfsPlus,false,true,dc42}).code,
                    "format a 400 KiB MFS image");
            auto image=HfsPlus::OpenImage(output.c_str(),false);
            FormatMfsSource source(*image);
            MfsImage volume;
            require(volume.Open(source)&&image->Size()==(dc42?400*1024+84+800*12:400*1024),
                    "formatted MFS image could not be opened at its selected size");
        }
        const auto home=folder+L"\\external.dsk";
        const auto journal=folder+L"\\external-journal.img";
        require(!run({home,journal,u"External",8*1024*1024}).code,"format image with external journal");
        {
            auto image=HfsPlus::OpenImage(home.c_str(),false,journal.c_str());
            require(HfsPlus::Volume(*image,false).ExternalJournal(),"formatted external journal did not reopen");
        }
        const auto declined=folder+L"\\declined.dsk";
        require(run({declined,journal,u"Guarded",8*1024*1024}).code&&
                GetFileAttributesW(declined.c_str())==INVALID_FILE_ATTRIBUTES&&
                GetFileAttributesW(journal.c_str())!=INVALID_FILE_ATTRIBUTES,
                "existing journal collision did not preserve the journal and clean up the new image");
        const auto failedHome=folder+L"\\failed.dsk";
        const auto failedJournal=folder+L"\\failed-journal.img";
        bool injected=false;
        HfsPlus::workerIoFault=[&](bool writing,uint64_t,size_t){
            if(writing&&!injected&&GetFileAttributesW(failedJournal.c_str())!=INVALID_FILE_ATTRIBUTES){
                injected=true;
                throw HfsPlus::Error("Injected external journal write failure");
            }
        };
        const auto failed=run({failedHome,failedJournal,u"Failure",8*1024*1024});
        HfsPlus::workerIoFault={};
        require(failed.code&&injected&&GetFileAttributesW(failedHome.c_str())==INVALID_FILE_ATTRIBUTES&&
                GetFileAttributesW(failedJournal.c_str())==INVALID_FILE_ATTRIBUTES,
                "external journal failure left outputs behind");
        fprintf(report,"PASS: all image formats, external journal reopen, existing journal preservation and rollback of both new outputs.\n");
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

std::shared_ptr<const Dmg::Password> PromptDmgPassword(CWnd* parent,bool retry)
{
    ImagePasswordDialog dialog(parent,retry);
    if(dialog.DoModal()!=IDOK)return {};
    return std::move(dialog.password);
}
namespace {
struct DmgConversion {
    std::wstring source,destination;
    Dmg::Output format;
    std::string expectedImage;
};
void StartDmgConversion(BrowserSession& session,DmgConversion paths,
                        std::shared_ptr<const Dmg::Password> password={})
{
    auto prompt=std::make_shared<std::optional<bool>>();
    auto complete=session.WhileOpen([paths,prompt](BrowserSession& origin,const Storage::Result& result){
        if(result.code)return;
        if(*prompt){
            if(auto supplied=PromptDmgPassword(origin.Owner(),**prompt))
                StartDmgConversion(origin,paths,std::move(supplied));
        }
    });
    BackgroundCopies::StartStorageResult([paths,prompt,password](Storage::Job& job){
        StorageLocation::ValidateImage(paths.source,paths.expectedImage);
        try{return Dmg::Convert(paths.source,paths.destination,paths.format,job,password.get());}
        catch(const Dmg::PasswordRequired& error){*prompt=error.retry;return Storage::Result{};}
    },session.Owner(),"Converting disk image","Convert disk image",std::move(complete),{true});
}
}

void ConvertDmgImage(BrowserSession& session)
{
    auto parent=session.Owner();
    std::wstring source,destination;
    if(!FileDialog(parent->m_hWnd,source,false,
        L"Disk images\0*.dmg;*.img;*.dsk;*.hfv;*.hfs;*.iso;*.smi;*.bin\0All files\0*.*\0\0",nullptr,
        L"Choose a disk image to convert"))return;
    DWORD type=1;
    if(!FileDialog(parent->m_hWnd,destination,true,
        L"Compressed DMG\0*.dmg\0Raw disk image\0*.img\0\0",nullptr,
        L"Save converted disk image",&type))return;
    const auto name=destination.substr(destination.find_last_of(L"\\/")+1);
    if(name.find(L'.')==name.npos)destination+=type==1?L".dmg":L".img";
    const auto format=type==1?Dmg::Output::CompressedDmg:Dmg::Output::RawImage;
    StartDmgConversion(session,{source,destination,format,VolumeCoord::ImageKey(source)});
}
