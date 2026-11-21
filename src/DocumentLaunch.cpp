// SPDX-License-Identifier: GPL-2.0-or-later
// Includes HFVExplorer launcher code by Lauri Pesonen, Copyright 1997-1998 Anygraaf Oy.
#include "stdafx.h"
#include "DocumentLaunch.h"
#include "BrowserPresentation.h"
#include "resource.h"
#include <filesystem>
#include "HFVExplorer.h"
#include "HFVExplorerDoc.h"
#include "BrowserSession.h"
#include "CopyProgress.h"
#include "LauncherWriteBack.h"
#include "FileOperations.h"
#include "StorageLocation.h"
#include "FileTypeMapping.h"
#include "StorageJobDialog.h"
#include "own_ids.h"
#include "utils.h"
#include "shell.h"
#include "icon.h"
#include "tmap.h"
#include "hfs/interface.h"
#include "special.h"
#include "charset.h"
#include "floppy.h"
#include <direct.h>
extern "C" {
#include "hfs/copyout.h"
}

void get_execute_error_string( HINSTANCE hi, CString &s )
{
	// cannot use this - ids change: s->LoadString( EXE_ERRBASE + (int)hi );
	switch( (INT_PTR)hi ) {
		case 0:
			s = "The operating system is out of memory or resources";
			break;
		case ERROR_FILE_NOT_FOUND:
			s = "The specified file was not found";
			break;
		case ERROR_PATH_NOT_FOUND	:
			s = "The specified path was not found";
			break;
		case ERROR_BAD_FORMAT	:
			s = "The .EXE file is invalid (non-Win32 .EXE or error in .EXE image)";
			break;
		case SE_ERR_ACCESSDENIED	:
			s = "The operating system denied access to the specified file.";
			break;
		case SE_ERR_ASSOCINCOMPLETE	:
			s = "The filename association is incomplete or invalid";
			break;
		case SE_ERR_DDEBUSY	:
			s = "The DDE transaction could not be completed because other DDE transactions were being processed";
			break;
		case SE_ERR_DDEFAIL	:
			s = "The DDE transaction failed";
			break;
		case SE_ERR_DDETIMEOUT	:
			s = "The DDE transaction could not be completed because the request timed out";
			break;
		case SE_ERR_DLLNOTFOUND	:
			s = "The specified dynamic-link library was not found.";
			break;

		/*
		case SE_ERR_FNF	:
			s = "The specified file was not found.";
			break;
		case SE_ERR_PNF	:
			s = "The specified path was not found";
			break;
		*/
		case SE_ERR_NOASSOC	:
			s = "There is no application associated with the given filename extension";
			break;
		case SE_ERR_OOM	:
			s = "There was not enough memory to complete the operation";
			break;
		case SE_ERR_SHARE	:
			s = "A sharing violation occurred";
			break;
		default:
			s = "Unknown error";
	}
}

namespace DocumentLaunch {
namespace {
using FileOperations::Location;

bool is_mac_path(const CString& path)
{
 return path.GetLength()<3||!isalpha(static_cast<unsigned char>(path[0]))||path[1]!=':'||path[2]!='\\';
}

void macish(CString* path)
{
 auto* text=path->GetBuffer();
 pc_to_mac_charset(reinterpret_cast<unsigned char*>(text));
 path->ReleaseBuffer();
}

class Launchers {
 BrowserSession& session_;
 const Request& request_;
public:
 Launchers(BrowserSession& session,const Request& request)
  :session_(session),request_(request){}
 bool Run();
private:
 bool MapDocumentType(const CString& type,const CString& creator);
 bool OpenClassicDocument(CString,CString);
};

bool Launchers::MapDocumentType(const CString& type,const CString& creator)
{
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 CFileTypeMapping dialog;
 tmap_final();
 dialog.m_enabled_param=app.m_type_mapping_enabled;
 dialog.m_input_types.Add(type);dialog.m_input_creators.Add(creator);dialog.m_input_dos.Add("???");
 const bool accepted=dialog.DoModal()==IDOK;
 if(accepted)app.m_type_mapping_enabled=dialog.m_enabled_param;
 tmap_init();
 return accepted;
}

bool OpenWindows(CWnd* owner,const Location& source,const Location* application)
{
 const auto argument=application?WindowsArgument(source):std::wstring();
 const auto& path=application?application->WindowsPath():source.WindowsPath();
 const auto result=ShellExecuteW(owner->GetSafeHwnd(),L"open",path.c_str(),
     application?argument.c_str():nullptr,nullptr,application?SW_SHOW:SW_SHOWNORMAL);
 if(INT_PTR(result)>32)return true;
 CString error;
 get_execute_error_string(result,error);
 owner->MessageBox(error,"Could not open the document",MB_OK|MB_ICONERROR);
 return false;
}

bool Launchers::OpenClassicDocument(CString type, CString creator)
{
 CString document;
 char extension[_MAX_PATH];

 const auto& selection=request_.selection;
 if(!selection.IsClassic()||selection.ClassicImage().size()>=_MAX_PATH)return FALSE;
 document=selection.ClassicPath().c_str();

 const char* old_extension=strrchr(static_cast<LPCSTR>(document),'.');

	if(!old_extension || strlen(old_extension) > 6) {
		if( !tmap_hfs2fat( (LPCSTR)type, (LPCSTR)creator, extension ) ) {
			AfxMessageBox(
				"There is no file type mapping from (" + type + "," + creator +
				"). Please define it in the dialog that will open next."
			);
			if(!MapDocumentType(type,creator)) {
				return(FALSE);
			}
		}

		if( !tmap_hfs2fat( (LPCSTR)type, (LPCSTR)creator, extension ) ) {
			return(FALSE);
		}
	}

 auto& session=session_;
 auto owner=session.Owner();
 std::string version;
 auto captured=ShowStorageWaitJob([&](Storage::Job& job){
  version=FileOperations::CaptureLaunchVersion(selection,job);
  return Storage::Result{};
 },!owner->IsWindowVisible(),owner);
 if(captured.code){
  owner->MessageBox(captured.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return FALSE;
 }
 CString staged;
 if(!StageWindowsLaunchDocument(session,selection,staged))return FALSE;
 CString directory=staged.Left(staged.ReverseFind(char(92)));
 DWORD attributes=FILE_ATTRIBUTE_TEMPORARY;
 if(mount_flags(const_cast<char*>(selection.ClassicImage().c_str()))==0)attributes|=FILE_ATTRIBUTE_READONLY;
 SetFileAttributes(staged,attributes);

 auto writeBack=std::make_unique<LauncherWriteBack>(selection,std::move(version),staged);
 if(!writeBack->CaptureBaseline(owner)){
  LauncherWriteBack::Discard(staged);
  return FALSE;
 }
 SHELLEXECUTEINFO launch{};
 launch.cbSize=sizeof(launch);
 launch.hwnd=owner->GetSafeHwnd();
 launch.lpVerb="open";
 launch.lpFile=staged;
 launch.lpDirectory=directory;
 launch.nShow=SW_SHOWNORMAL;
 launch.fMask=SEE_MASK_NOCLOSEPROCESS;
 BOOL launched=ShellExecuteEx(&launch);
 writeBack->Track(launch.hProcess);
 if(!launched||reinterpret_cast<INT_PTR>(launch.hInstApp)<=32){
  owner->MessageBox("Could not open "+staged,"HFVExplorer",MB_OK|MB_ICONERROR);
  LauncherWriteBack::Discard(staged);
  return FALSE;
 }
 static_cast<CMainFrame*>(owner)->TrackLaunchedDocument(std::move(writeBack));
 return TRUE;
}

bool Launchers::Run()
{
 StorageLocation::RequireCapturedIdentity(request_.selection);
 char typeBytes[5]{},creatorBytes[5]{};
 memcpy(typeBytes,request_.entry.finder.data(),4);memcpy(creatorBytes,request_.entry.finder.data()+4,4);
 const CString type(typeBytes), creator(creatorBytes);
 if(request_.selection.IsWindows())
  return OpenWindows(session_.Owner(),request_.selection,nullptr);
 if(type=="APPL"){
  AfxMessageBox("Mac applications can be opened from inside an emulator.");
  return false;
 }
 return OpenClassicDocument(type,creator);
}
}

bool Launch(BrowserSession& session,Request request)
{
 try{return Launchers(session,request).Run();}
 catch(const std::exception& error){session.Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return false;}
}

bool OpenWith(BrowserSession& session,const Location& source,const Location& application)
{
 return OpenWindows(session.Owner(),source,&application);
}

bool SaveFileIcon(HICON small,HICON large,const char* path,bool document)
{
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 if(!small)small=large?large:app.LoadIcon(document?IDI_ICON9:IDI_ICON5);
 if(!large)large=small;
 return small&&large&&SaveIcon(small,large,const_cast<char*>(path))!=0;
}

std::wstring WindowsArgument(const Location& source)
{
 return L"\""+source.WindowsPath()+L"\"";
}
}

namespace DocumentLaunch {
namespace {
enum class Emulator { BasiliskII, MiniVMac };

std::wstring EmulatorCommand(Emulator emulator, const std::wstring& executable, const std::wstring& input)
{
    // Validated file paths cannot end in a separator or contain a quote.
    auto command = L"\"" + executable + L"\"";
    if (emulator == Emulator::BasiliskII)
        command += L" --config";
    if (!input.empty())
        command += L" \"" + input + L"\"";
    return command;
}

DWORD StartEmulator(Emulator emulator, const std::wstring& executable, const std::wstring& input,
                    int& invalidField)
{
    invalidField = 0;
    for (const auto& file : {std::pair{IDC_EMULATOR_EXECUTABLE, &executable},
                             std::pair{IDC_EMULATOR_INPUT, &input}}) {
        const auto& path = *file.second;
        if (file.first == IDC_EMULATOR_INPUT && emulator == Emulator::MiniVMac && path.empty())
            continue;
        if (!std::filesystem::path(path).is_absolute() ||
            path.find_first_of(L"\"\r\n") != std::wstring::npos) {
            invalidField = file.first;
            return ERROR_INVALID_NAME;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            invalidField = file.first;
            return error;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || path.back() == L'\\' || path.back() == L'/') {
            invalidField = file.first;
            return ERROR_INVALID_NAME;
        }
    }

    if (!input.empty()) {
        const HANDLE file = CreateFileW(input.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            invalidField = IDC_EMULATOR_INPUT;
            return error;
        }
        CloseHandle(file);
    }

    auto command = EmulatorCommand(emulator, executable, input);
    const auto directory = std::filesystem::path(executable).parent_path();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, directory.c_str(), &startup, &child))
        return GetLastError();
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return ERROR_SUCCESS;
}

class EmulatorDialog final : public CDialog {
public:
    EmulatorDialog(Emulator emulator, CWnd* owner)
        : CDialog(Template(emulator), owner), emulator_(emulator) {}

    static UINT Template(Emulator emulator)
    {
        return emulator == Emulator::MiniVMac ? IDD_MINI_VMAC : IDD_BASILISK_II;
    }

protected:
    BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();
        InitPath(IDC_EMULATOR_EXECUTABLE, "Executable");
        InitPath(IDC_EMULATOR_INPUT, InputKey());
        return TRUE;
    }

    BOOL OnCommand(WPARAM w, LPARAM l) override
    {
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_EMULATOR_BROWSE_EXECUTABLE) {
            Browse(IDC_EMULATOR_EXECUTABLE, L"Programs (*.exe)\0*.exe\0All files\0*.*\0");
            return TRUE;
        }
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) == IDC_EMULATOR_BROWSE_INPUT) {
            Browse(IDC_EMULATOR_INPUT, IsMini() ?
                L"Disk images (*.dsk;*.img;*.hfv;*.dc42)\0*.dsk;*.img;*.hfv;*.dc42\0All files\0*.*\0" :
                L"All files\0*.*\0");
            return TRUE;
        }
        return CDialog::OnCommand(w, l);
    }

    void OnOK() override
    {
        const auto executable = Path(IDC_EMULATOR_EXECUTABLE);
        const auto input = Path(IDC_EMULATOR_INPUT);
        int invalidField;
        const auto error = StartEmulator(emulator_, executable, input, invalidField);
        if (error != ERROR_SUCCESS) {
            std::wstring message;
            if (invalidField == IDC_EMULATOR_EXECUTABLE)
                message = L"Choose an existing emulator executable.";
            else if (invalidField == IDC_EMULATOR_INPUT)
                message = IsMini() ? L"Choose an existing, readable disk image or leave it blank."
                                   : L"Choose an existing, readable Basilisk II preferences file.";
            else
                message = L"Windows could not start the emulator.";
            message += L"\n\nWindows error " + std::to_wstring(error) + L".";
            ::MessageBoxW(m_hWnd, message.c_str(), IsMini() ? L"Launch Mini vMac" : L"Launch Basilisk II",
                          MB_OK | MB_ICONERROR);
            if (invalidField)
                GetDlgItem(invalidField)->SetFocus();
            return;
        }
        SavePath("Executable", executable);
        SavePath(InputKey(), input);
        CDialog::OnOK();
    }

private:
    Emulator emulator_;
    bool IsMini() const { return emulator_ == Emulator::MiniVMac; }
    const char* Profile() const { return IsMini() ? "MiniVMac" : "BasiliskII"; }
    const char* InputKey() const { return IsMini() ? "DiskImage" : "Preferences"; }

    void InitPath(int id, const char* key)
    {
        BrowserPresentation::UnicodeEdit(m_hWnd, id);
        const auto value = AfxGetApp()->GetProfileString(Profile(), key);
        ::SetDlgItemTextW(m_hWnd, id, BrowserPresentation::FromUtf8(static_cast<LPCSTR>(value)).c_str());
    }

    void SavePath(const char* key, const std::wstring& path) const
    {
        const auto encoded = BrowserPresentation::Utf8(MacBrowser::U16(path));
        AfxGetApp()->WriteProfileString(Profile(), key, encoded.c_str());
    }

    std::wstring Path(int id) const
    {
        return BrowserPresentation::Text(::GetDlgItem(m_hWnd, id));
    }

    void Browse(int id, const wchar_t* filter)
    {
        std::wstring buffer(32768, L'\0');
        wcsncpy_s(buffer.data(), buffer.size(), Path(id).c_str(), _TRUNCATE);
        OPENFILENAMEW file{sizeof(file)};
        file.hwndOwner = m_hWnd;
        file.lpstrFile = buffer.data();
        file.nMaxFile = static_cast<DWORD>(buffer.size());
        file.lpstrFilter = filter;
        file.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&file))
            ::SetDlgItemTextW(m_hWnd, id, buffer.c_str());
    }
};
}

void ShowBasiliskII(CWnd* owner)
{
    EmulatorDialog(Emulator::BasiliskII, owner).DoModal();
}

void ShowMiniVMac(CWnd* owner)
{
    EmulatorDialog(Emulator::MiniVMac, owner).DoModal();
}

#ifdef HFV_TESTING
bool ProbeEmulatorLaunchers(CWnd* owner, const char* directory)
{
    const auto base = std::filesystem::path(directory);
    const auto executable = (base / L"Emulator test.exe").wstring();
    const auto input = (base / L"System \u65e5\u672c.dsk").wstring();
    const auto checkArguments = [](const std::wstring& command,
                                   std::initializer_list<std::wstring> expected) {
        int count = 0;
        auto argv = CommandLineToArgvW(command.c_str(), &count);
        if (!argv)
            return false;
        const bool match = count == static_cast<int>(expected.size()) &&
                           std::equal(expected.begin(), expected.end(), argv);
        LocalFree(argv);
        return match;
    };
    if (!checkArguments(EmulatorCommand(Emulator::BasiliskII, executable, input),
                        {executable, L"--config", input}) ||
        !checkArguments(EmulatorCommand(Emulator::MiniVMac, executable, input),
                        {executable, input}) ||
        !checkArguments(EmulatorCommand(Emulator::MiniVMac, executable, {}),
                        {executable}))
        return false;

    int invalidField;
    for (const auto emulator : {Emulator::BasiliskII, Emulator::MiniVMac}) {
        EmulatorDialog dialog(emulator, owner);
        if (!dialog.Create(EmulatorDialog::Template(emulator), owner))
            return false;
        const bool controls = IsWindowUnicode(::GetDlgItem(dialog.m_hWnd, IDC_EMULATOR_EXECUTABLE)) &&
                              IsWindowUnicode(::GetDlgItem(dialog.m_hWnd, IDC_EMULATOR_INPUT)) &&
                              dialog.GetDlgItem(IDC_EMULATOR_BROWSE_EXECUTABLE) &&
                              dialog.GetDlgItem(IDC_EMULATOR_BROWSE_INPUT);
        dialog.DestroyWindow();
        if (!controls || StartEmulator(emulator, executable, input, invalidField) == ERROR_SUCCESS ||
            invalidField != IDC_EMULATOR_EXECUTABLE)
            return false;
    }

    const auto writeFixture = [](const std::wstring& path) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") || !file)
            return false;
        const bool wrote = fputs("fixture", file) >= 0;
        return fclose(file) == 0 && wrote;
    };
    if (!writeFixture(executable))
        return false;
    for (const auto emulator : {Emulator::BasiliskII, Emulator::MiniVMac}) {
        if (StartEmulator(emulator, executable, input, invalidField) == ERROR_SUCCESS ||
            invalidField != IDC_EMULATOR_INPUT)
            return false;
    }
    if (StartEmulator(Emulator::BasiliskII, executable, {}, invalidField) == ERROR_SUCCESS ||
        invalidField != IDC_EMULATOR_INPUT ||
        StartEmulator(Emulator::MiniVMac, executable, {}, invalidField) == ERROR_SUCCESS ||
        invalidField || !writeFixture(input))
        return false;
    return StartEmulator(Emulator::MiniVMac, executable, input, invalidField) != ERROR_SUCCESS &&
           invalidField == 0;
}
#endif
}
