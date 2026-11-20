// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CommandStorage.h"
#include "MainMacBrowser.h"
#include "BrowserSession.h"
#include "resource.h"
#include "HfsPlus.h"
#include "HfsPlusDialogs.h"
#include "ApfsFormat.h"
#include "HfsPlusDisk.h"
#include "DmgImage.h"
#include "SparseDisk.h"
#include "DmgEncryption.h"
#include "DmgCertificate.h"
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
    bool retry_,keyFile_;
    const std::wstring* apfsHint_;
    void Clear() {
        SetDlgItemTextW(m_hWnd,IDC_DMG_PASSWORD,L"");
        SendDlgItemMessage(IDC_DMG_PASSWORD,EM_EMPTYUNDOBUFFER,0,0);
    }
public:
    std::shared_ptr<const Dmg::Password> password;
    ImagePasswordDialog(CWnd* parent,bool retry,bool keyFile=false,const std::wstring* apfsHint=nullptr)
        :CDialog(IDD_DMG_PASSWORD,parent),retry_(retry),keyFile_(keyFile),apfsHint_(apfsHint){}
    BOOL OnInitDialog()override{
        CDialog::OnInitDialog();
        UnicodeEdit(m_hWnd,IDC_DMG_PASSWORD);
        SendDlgItemMessage(IDC_DMG_PASSWORD,EM_SETLIMITTEXT,1023);
        if(keyFile_)SetDlgItemTextW(m_hWnd,IDC_DMG_PROMPT,retry_?
            L"The key file could not be opened. Try another password.":L"Enter the password for this PFX or P12 key file.");
        else if(apfsHint_){
            SetWindowTextW(m_hWnd,L"Unlock APFS volume");
            GetDlgItem(IDC_DMG_PROMPT)->ModifyStyle(0,SS_NOPREFIX);
            std::wstring prompt=retry_?L"Incorrect APFS password. Try again.":L"Enter the password for this APFS volume.";
            if(!apfsHint_->empty())prompt+=L"\r\nHint: "+*apfsHint_;
            SetDlgItemTextW(m_hWnd,IDC_DMG_PROMPT,prompt.c_str());
        }
        else if(retry_)SetDlgItemTextW(m_hWnd,IDC_DMG_PROMPT,L"The password did not unlock this image. Try again.");
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
    ApfsImageMode apfs=ApfsImageMode::None;
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
             {"HFS+ (case-insensitive)", "HFSX (case-sensitive)", "HFSX (case-insensitive)",
              "MFS (raw image)", "MFS (Disk Copy 4.2)", "APFS (case-insensitive)", "APFS (case-sensitive)"})
            SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, 0);
        return TRUE;
    }
    BOOL OnCommand(WPARAM wp,LPARAM lp)override{
        if(LOWORD(wp)==IDC_HPLUS_FORMAT_KIND&&HIWORD(wp)==CBN_SELCHANGE){
            const int selected=int(SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_GETCURSEL));
            const bool nowMfs=selected==3||selected==4;
            const bool hfsPlus=selected>=0&&selected<=2;
            GetDlgItem(IDC_HPLUS_JOURNAL)->EnableWindow(hfsPlus);
            GetDlgItem(IDC_HPLUS_EXPANDED)->EnableWindow(hfsPlus);
            GetDlgItem(IDC_HPLUS_EXTERNAL)->EnableWindow(hfsPlus);
            if(nowMfs!=mfs)SetDlgItemText(IDC_HPLUS_SIZE,nowMfs?"400 KiB":"128 MiB");
            mfs=nowMfs;
            return TRUE;
        }
        if(LOWORD(wp)==IDC_HPLUS_EXTERNAL&&IsDlgButtonChecked(IDC_HPLUS_EXTERNAL)==BST_CHECKED)CheckDlgButton(IDC_HPLUS_JOURNAL,BST_CHECKED);
        return CDialog::OnCommand(wp,lp);
    }
    void OnOK() override {
        try {
            const int selected=int(SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_GETCURSEL));
            mfs=selected==3||selected==4;
            dc42=selected==4;
            apfs=selected==5?ApfsImageMode::CaseInsensitive:selected==6?ApfsImageMode::CaseSensitive:ApfsImageMode::None;
            if(mfs){std::string encoded,error;auto text=Text(m_hWnd,IDC_HPLUS_NAME);if(!MfsImage::EncodeName(text,encoded,error,27))throw HfsPlus::Error(error);name=U16(text);}
            else if(apfs!=ApfsImageMode::None){
                name=U16(Text(m_hWnd, IDC_HPLUS_NAME));
                Apfs::FormatLabel(name);
            }else {kind = SelectedKind();
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
            const bool hfsPlus=!mfs&&apfs==ApfsImageMode::None;
            journaled=hfsPlus&&IsDlgButtonChecked(IDC_HPLUS_JOURNAL)==BST_CHECKED;
            expanded=hfsPlus&&IsDlgButtonChecked(IDC_HPLUS_EXPANDED)==BST_CHECKED;
            external=hfsPlus&&IsDlgButtonChecked(IDC_HPLUS_EXTERNAL)==BST_CHECKED;
            if(external&&!journaled)throw HfsPlus::Error("Enable journaling to use an external journal");
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
        const bool apfs=request.apfs!=ApfsImageMode::None;
        if(apfs&&(request.mfs||request.dc42))
            return Storage::Result{1,ERROR_INVALID_PARAMETER,"Choose exactly one image format."};
        if(external&&(request.mfs||apfs||!request.journaled))
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
        return CommandStorage::Guard(std::move(access),[&](Storage::Job& job){
            constexpr uint64_t journalSize=64ULL*1024*1024;
            std::unique_ptr<HfsPlus::NewImage> home,journal;
            try{
                const auto tags=request.dc42&&(request.size==400*1024||request.size==800*1024)?request.size/512*12:0;
                const auto bytes=request.size+(request.dc42?84+tags:0);
                home=apfs?HfsPlus::CreateStagedOutput(request.path,bytes):
                     HfsPlus::CreateFormatOutput(request.path.c_str(),bytes,external?request.externalJournal.c_str():nullptr);
                if(apfs){
                    Apfs::Format(*home,request.name,request.apfs==ApfsImageMode::CaseSensitive,job);
                    if(job.cancelled())throw Storage::Cancelled();
                    home->PublishTo(request.path);
                }else if(request.mfs){
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
                    options.apfs!=ApfsImageMode::None?L"img":L"dsk"))
        return;
    std::wstring externalPath;
    if(options.external&&!FileDialog(parent->m_hWnd,externalPath,true,L"External journal disk image\0*.img\0All files\0*.*\0\0",L"img"))return;
    StartMacImageFormat(session,{std::move(path),std::move(externalPath),std::move(options.name),options.size,
                                options.kind,options.journaled,options.mfs,options.dc42,options.expanded,options.apfs});
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
        require(format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_GETCOUNT) == 7,
                "Mac image format choices missing");
        for (int choice = 0; choice < 3; ++choice) {
            format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND, CB_SETCURSEL, choice);
            require(static_cast<int>(format.SelectedKind()) == choice, "Format choice not applied");
        }
        for(int choice=3;choice<5;++choice){format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_SETCURSEL,choice);format.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_HPLUS_FORMAT_KIND,CBN_SELCHANGE),0);require(!format.GetDlgItem(IDC_HPLUS_EXTERNAL)->IsWindowEnabled()&&!format.GetDlgItem(IDC_HPLUS_EXPANDED)->IsWindowEnabled()&&!format.GetDlgItem(IDC_HPLUS_JOURNAL)->IsWindowEnabled()&&Text(format.m_hWnd,IDC_HPLUS_SIZE)==L"400 KiB","MFS formatter defaults/journal state");}
        for(int choice=5;choice<7;++choice){
            format.SendDlgItemMessage(IDC_HPLUS_FORMAT_KIND,CB_SETCURSEL,choice);
            format.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_HPLUS_FORMAT_KIND,CBN_SELCHANGE),0);
            require(!format.GetDlgItem(IDC_HPLUS_EXTERNAL)->IsWindowEnabled()&&
                    !format.GetDlgItem(IDC_HPLUS_EXPANDED)->IsWindowEnabled()&&
                    !format.GetDlgItem(IDC_HPLUS_JOURNAL)->IsWindowEnabled()&&
                    Text(format.m_hWnd,IDC_HPLUS_SIZE)==L"128 MiB","APFS format size and options");
        }
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
        for(const bool sensitive:{false,true}){
            const auto output=folder+(sensitive?L"\\formatted-apfs-sensitive.img":L"\\formatted-apfs.img");
            MacImageFormatRequest request;
            request.path=output;request.size=sensitive?129ULL*1024*1024:8ULL*1024*1024;
            request.name=u"New APFS \u65e5\u672c";
            request.apfs=sensitive?ApfsImageMode::CaseSensitive:ApfsImageMode::CaseInsensitive;
            const auto result=run(request);
            require(!result.code,result.message.c_str());
            auto volume=MacBrowser::OpenBrowserImage(output);
            require(volume&&std::string(volume->Kind())=="APFS"&&volume->Name()==request.name&&
                    volume->CaseSensitive()==sensitive&&volume->List(2).empty()&&volume->Writable(),
                    "Created APFS image did not reopen writable with its name and case mode");
            volume.reset(); // Release the exclusive browser handle before reading raw bytes.
            const auto original=HfsPlus::Disk::Read(*HfsPlus::OpenImage(output.c_str(),false),0,4096);
            require(run(request).code&&HfsPlus::Disk::Read(*HfsPlus::OpenImage(output.c_str(),false),0,4096)==original,
                    "Existing APFS image was overwritten");
        }
        MacImageFormatRequest failedApfs;
        failedApfs.name=u"Failed APFS";
        failedApfs.size=8*1024*1024;
        failedApfs.apfs=ApfsImageMode::CaseInsensitive;
        failedApfs.path=folder+L"\\failed-apfs.img";
        bool faulted=false;
        HfsPlus::workerIoFault=[&](bool writing,uint64_t at,size_t){
            if(writing&&at==0){
                faulted=true;
                throw HfsPlus::Error("Injected APFS superblock write failure");
            }
        };
        const auto writeFailure=run(failedApfs);
        HfsPlus::workerIoFault={};
        require(writeFailure.code&&faulted&&GetFileAttributesW(failedApfs.path.c_str())==INVALID_FILE_ATTRIBUTES,
                "Failed APFS formatting left output behind");

        Storage::Job cancelled;
        failedApfs.path=folder+L"\\cancelled-apfs.img";
        HfsPlus::workerIoFault=[&](bool writing,uint64_t,size_t){if(writing)cancelled.cancel();};
        bool refused=false;
        try{MakeMacImageFormatJob(failedApfs)(cancelled);}
        catch(const Storage::Cancelled&){refused=true;}
        catch(...){HfsPlus::workerIoFault={};throw;}
        HfsPlus::workerIoFault={};
        require(refused&&GetFileAttributesW(failedApfs.path.c_str())==INVALID_FILE_ATTRIBUTES,
                "Cancelled APFS formatting left output behind");
        WIN32_FIND_DATAW staged{};
        const auto leftover=FindFirstFileW((folder+L"\\.hfv-image-*.tmp").c_str(),&staged);
        if(leftover!=INVALID_HANDLE_VALUE)FindClose(leftover);
        require(leftover==INVALID_HANDLE_VALUE,"APFS formatting left a temporary image behind");
        fprintf(report,"PASS: APFS case modes, Unicode name, multiple allocation chunks, no overwrite, and failed or cancelled format cleanup.\n");
        if(_wgetenv(L"HFV_APFS_LARGE_FORMAT")){
            for(const auto& geometry:{std::pair{L"\\formatted-apfs-multi-cib.img",17ULL<<30},
                                      std::pair{L"\\formatted-apfs-multi-cab.img",8ULL<<40}}){
                auto request=failedApfs;
                request.path=folder+geometry.first;
                request.name=u"Large APFS";
                request.size=geometry.second;
                const auto result=run(request);
                require(!result.code,result.message.c_str());
                auto volume=MacBrowser::OpenBrowserImage(request.path);
                require(volume&&volume->Name()==request.name&&volume->List(2).empty()&&
                        volume->Writable()==(request.size==(17ULL<<30)),
                        "Large APFS image did not reopen with the expected write capability");
            }
            fprintf(report,"PASS: sparse 17 GiB and 8 TiB APFS geometries across multiple chunk-info and address blocks.\n");
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
        fprintf(report, "PASS: active Unicode name and formatter dialogs; HFS+/HFSX/MFS/APFS format choices, "
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

Dmg::Credentials PromptDmgCredentials(CWnd* parent,Dmg::UnlockMethod method,bool retry,const std::wstring* apfsHint)
{
    using Method=Dmg::UnlockMethod;
    if(method==Method::Either){
        const int choice=MessageBoxW(parent->m_hWnd,retry?
            L"The previous credential did not unlock the image.\n\nChoose Yes to enter a password or No to select a private key.":
            L"Choose Yes to enter a password or No to select a private key.",
            L"Unlock disk image",MB_YESNOCANCEL|MB_ICONQUESTION);
        if(choice!=IDYES&&choice!=IDNO)return {};
        method=choice==IDYES?Method::Password:Method::PrivateKey;
    }
    if(method==Method::Password){
        ImagePasswordDialog dialog(parent,retry,false,apfsHint);
        if(dialog.DoModal()!=IDOK)return {};
        return {std::move(dialog.password),{}};
    }
    std::wstring path;
    if(!FileDialog(parent->m_hWnd,path,false,
                   L"Private key files\0*.pem;*.der;*.key;*.pfx;*.p12\0All files\0*.*\0\0",nullptr,
                   retry?L"Select a different RSA private key":L"Select the RSA private key for this disk image"))return {};
    std::shared_ptr<const Dmg::Password> password;
    for(;;){
        try{return {{},std::make_shared<Dmg::PrivateKey>(path,password.get())};}
        catch(const Dmg::KeyFilePasswordRequired&){
            ImagePasswordDialog dialog(parent,bool(password),true);
            if(dialog.DoModal()!=IDOK)return {};
            password=std::move(dialog.password);
        }catch(const std::exception& error){
            const auto message=VolumeCoord::Wide(error.what());
            MessageBoxW(parent->m_hWnd,message.c_str(),L"Cannot open private key",MB_OK|MB_ICONERROR);
            return {};
        }
    }
}
namespace {
struct DmgConversion {
    std::wstring source,destination;
    Dmg::Output format;
    std::string expectedImage;
};
void StartDmgConversion(BrowserSession& session,DmgConversion paths,
                        Dmg::Credentials credentials={})
{
    auto prompt=std::make_shared<std::optional<Dmg::PasswordRequired>>();
    auto complete=session.WhileOpen([paths,prompt](BrowserSession& origin,const Storage::Result& result){
        if(result.code)return;
        if(*prompt){
            if(auto supplied=PromptDmgCredentials(origin.Owner(),(*prompt)->method,(*prompt)->retry))
                StartDmgConversion(origin,paths,std::move(supplied));
        }
    });
    BackgroundCopies::StartStorageResult([paths,prompt,credentials](Storage::Job& job){
        StorageLocation::ValidateImage(paths.source,paths.expectedImage);
        try{return Dmg::Convert(paths.source,paths.destination,paths.format,job,credentials.password.get(),credentials.privateKey.get());}
        catch(const Dmg::PasswordRequired& error){*prompt=error;return Storage::Result{};}
    },session.Owner(),"Converting disk image","Convert disk image",std::move(complete),{true});
}
}

void ConvertDmgImage(BrowserSession& session)
{
    auto parent=session.Owner();
    std::wstring source,destination;
    if(!FileDialog(parent->m_hWnd,source,false,
        L"Disk images\0*.dmg;*.img;*.dsk;*.hfv;*.hfs;*.iso;*.sparseimage;*.smi;*.bin;*.hqx;*.as\0Sparse bundle metadata (Info.plist)\0Info.plist\0All files\0*.*\0\0",nullptr,
        L"Choose a disk image to convert"))return;
    source=SparseDisk::BundleFromPlist(source);
    DWORD type=1;
    if(!FileDialog(parent->m_hWnd,destination,true,
        L"Compressed DMG\0*.dmg\0Raw disk image\0*.img\0NDIF (compressed, MacBinary)\0*.bin\0NDIF (uncompressed, MacBinary)\0*.bin\0NDIF (read/write, MacBinary)\0*.bin\0\0",nullptr,
        L"Save converted disk image",&type))return;
    const auto name=destination.substr(destination.find_last_of(L"\\/")+1);
    if(name.find(L'.')==name.npos)destination+=type==1?L".dmg":type==2?L".img":L".bin";
    constexpr Dmg::Output formats[]={Dmg::Output::CompressedDmg,Dmg::Output::RawImage,
                                     Dmg::Output::CompressedNdif,Dmg::Output::StoredNdif,Dmg::Output::EditableNdif};
    if(type<1||type>_countof(formats))return;
    const auto format=formats[type-1];
    StartDmgConversion(session,{source,destination,format,VolumeCoord::ImageKey(source)});
}
