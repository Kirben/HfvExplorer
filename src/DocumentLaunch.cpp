// SPDX-License-Identifier: GPL-2.0-or-later
// Includes HFVExplorer launcher code by Lauri Pesonen, Copyright 1997-1998 Anygraaf Oy.
#include "stdafx.h"
#include "DocumentLaunch.h"
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

CString make_mac_chrset(CString path)
{
 macish(&path);
 CString encoded;
 for(int index=0;index<path.GetLength();++index){
  const auto ch=static_cast<unsigned char>(path[index]);
  if(isalnum(ch)||strchr(" !#&/()=?.,;:-",ch))encoded+=char(ch);
  else{char hex[5];sprintf_s(hex,"::%02x",unsigned(ch));encoded+=hex;}
 }
 return encoded;
}
void make_short_name(CString& path);

std::pair<CString,CString> SplitExecutorCommand(CString command)
{
 command.Trim();
 if(command.IsEmpty())return {};
 if(command[0]=='"'){
  const int end=command.Find('"',1);
  if(end<0)throw std::invalid_argument("Executor executable path is missing a closing quote");
  return {command.Mid(1,end-1),command.Mid(end+1).TrimLeft()};
 }
 const int end=command.FindOneOf(" \t");
 if(end<0)return {command,{}};
 return {command.Left(end),command.Mid(end).TrimLeft()};
}

CString ExecutorArguments(CString command,CString document,CString application,bool dos)
{
 auto argument=[dos](CString path){
  if(is_mac_path(path))return make_mac_chrset(path);
  macish(&path);path.OemToAnsi();
  CString drive=path.Left(1);drive.MakeLower();
  path=drive+path.Mid(1);
  if(dos)make_short_name(path);
  return path;
 };
 if(!command.IsEmpty()&&command.Right(1)!=" ")command+=" ";
 if(!application.IsEmpty())command+="\""+argument(application)+"\" ";
 return command+"\""+argument(document)+"\"";
}

struct Settings {
 CString m_executor_dos_path,m_executor_win32_path,m_vmac_path,m_vmac_startup_folder_name,m_vmac_system_file_path;
 CString m_shapeshifter_path,m_shapeshifter_startup_folder_name,m_shapeshifter_system_file_path,m_shapeshifter_preferences_file_path;
 CString m_fusion_startup_folder_name,m_fusion_system_file_path;
 int m_launch_method,m_link_name_style,defaultEmulator;
 explicit Settings(const CHFVExplorerApp& app)
  :m_executor_dos_path(app.m_executor_dos_path),m_executor_win32_path(app.m_executor_win32_path),
   m_vmac_path(app.m_vmac_path),m_vmac_startup_folder_name(app.m_vmac_startup_folder_name),
   m_vmac_system_file_path(app.m_vmac_system_file_path),m_shapeshifter_path(app.m_shapeshifter_path),
   m_shapeshifter_startup_folder_name(app.m_shapeshifter_startup_folder_name),
   m_shapeshifter_system_file_path(app.m_shapeshifter_system_file_path),
   m_shapeshifter_preferences_file_path(app.m_shapeshifter_preferences_file_path),
   m_fusion_startup_folder_name(app.m_fusion_startup_folder_name),m_fusion_system_file_path(app.m_fusion_system_file_path),
   m_launch_method(app.m_launch_method),m_link_name_style(app.m_link_name_style),defaultEmulator(app.m_default_emulator){}
};

class Launchers {
 BrowserSession& session_;
 const Request& request_;
 Settings settings_;
public:
 Launchers(BrowserSession& session,const Request& request)
  :session_(session),request_(request),settings_(*static_cast<CHFVExplorerApp*>(AfxGetApp())){}
 bool Run();
private:
 bool LegacyDocument(CString& document)const;
 bool MapDocumentType(const CString& type,const CString& creator);
 void get_icons_folder_name(char*);
 void get_vmac_launcher_name(char*);
 int make_icon_name(LPSTR);
 int make_e_dos_param_file_name(LPSTR);
 int check_shortcut_installation(Target);
 int write_text_file(LPSTR,LPSTR);
 void add_link_tail(char*,char*);
 int do_make_shortcut(CString,CString,Target,int);
 int do_make_shortcut_e_dos(CString,CString,int);
 int do_make_shortcut_e_win32(CString,CString,int);
 int do_make_shortcut_windows(CString,CString,int);
 int do_make_shortcut_vmac(CString,CString,int);
 int do_make_shortcut_ss(CString,CString,int);
 int do_make_shortcut_fusion(CString,CString,int);
};

bool Launchers::LegacyDocument(CString& document)const
{
 document=request_.selection.LegacyPath().c_str();
 return !document.IsEmpty()&&document.GetLength()<MAX_PATH;
}

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

bool RunExecutor(CWnd* owner,const CString& configured,const Location& source,CString application)
{
 const auto command=ExecutorCommand(configured,source,std::move(application));
 if(WinExec(command,SW_SHOW)>31)return true;
 owner->MessageBox("Could not start "+command+
     ".\nPlease use execut95.exe to define Executor path and options.","HFVExplorer",MB_OK|MB_ICONERROR);
 return false;
}

void Launchers::get_icons_folder_name( char *name )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	char *p;

	GetModuleFileName( pApp->m_hInstance, name, _MAX_PATH );
	p = strrchr( name, '\\' );
	if(p) *p = 0;
	strcat( name, "\\Links" );
}

void Launchers::get_vmac_launcher_name( char *name )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	char *p;

	GetModuleFileName( pApp->m_hInstance, name, _MAX_PATH );
	p = strrchr( name, '\\' );
	if(p) *p = 0;
	strcat( name, "\\vMacLink.exe" );
}

int Launchers::make_icon_name( LPSTR lpszIconFile )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	char folder[_MAX_PATH];

	*lpszIconFile = 0;
	get_icons_folder_name( folder );
	if(!exists(folder)) {
		if(!CreateDirectory(folder,NULL)) {
			CString cs = "Cannot create the icon folder \"" + CString(folder) + "\".";
			AfxMessageBox( cs );
			return(0);
		} else {
			CString cs = "The folder \"" + CString(folder) + "\" was created. "
				"All shortcut icons (and E/DOS parameter files) will be in this folder.";
			AfxMessageBox( cs );
		}
	}
	int i = 0;
	do {
		sprintf(
			lpszIconFile,
			"%s\\Ic%06d.ico",
			folder,
			++pApp->m_next_icon_index );
			if(++i > 1000) break; // panic out, use this whatever icon it was.
	} while( exists(lpszIconFile) );
	pApp->WriteIconIndex();
	return(1);
}

// assumes that "make_icon_name" was just called!
int Launchers::make_e_dos_param_file_name( LPSTR lpszParamsFile )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	char folder[_MAX_PATH];

	*lpszParamsFile = 0;
	get_icons_folder_name( folder );
	if(!exists(folder)) {
		CString cs = "Folder \"" + CString(folder) + "\" does not exist.";
		AfxMessageBox( cs );
		return(0);
	}
	sprintf(
		lpszParamsFile,
		"%s\\Ic%06d.txt",
		folder,
		pApp->m_next_icon_index );
	return(1);
}

void pc_validate_charset( LPSTR s )
{
	int i, len = strlen(s);

	for(i=0; i<len; i++) {
		switch( s[i] ) {
			case '\\': s[i] = '_'; break;
			case '/':  s[i] = '-'; break;
			case ':':  s[i] = ';'; break;
			case '*':  s[i] = '@'; break;
			case '?':  s[i] = '!'; break;
			case '\"': s[i] = '-'; break;
			case '<':  s[i] = '['; break;
			case '>':  s[i] = ']'; break;
			case '|':  s[i] = '-'; break;
		}
	}
}

void make_shortcut_name( LPCSTR objpath, LPSTR linkname )
{
	const char *p;
	int ismac;

	*linkname = 0;

	ismac = is_mac_path(CString(objpath));
	if(ismac) {
		p = strrchr( objpath, ':' );
	} else {
		p = strrchr( objpath, '\\' );
	}
	if(p) {
		p++;
		strcpy( linkname, p );
		if(ismac) {
			pc_validate_charset( linkname );
		} else {
			char *extension = strrchr( linkname, '.' );
			if(extension) *extension = 0;
		}
	}
}

int Launchers::check_shortcut_installation(Target emulator)
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();

	switch( emulator ) {
		case Target::ExecutorDos:
			if(settings_.m_executor_dos_path == "") return(0);
			break;
		case Target::ExecutorWin32:
			if(settings_.m_executor_win32_path == "") return(0);
			break;
		case Target::VMac:
			if(settings_.m_vmac_path == "") return(0);
			if(settings_.m_vmac_startup_folder_name == "") return(0);
			if(settings_.m_vmac_system_file_path == "") return(0);
			break;
		case Target::ShapeShifter:
			if(settings_.m_shapeshifter_path == "") return(0);
			if(settings_.m_shapeshifter_startup_folder_name == "") return(0);
			if(settings_.m_shapeshifter_system_file_path == "") return(0);
			if(settings_.m_shapeshifter_preferences_file_path == "") return(0);
			break;
		case Target::Fusion:
			if(settings_.m_fusion_startup_folder_name == "") return(0);
			if(settings_.m_fusion_system_file_path == "") return(0);
			break;
		case Target::Windows:
			return(1);
			break;
	}
	return(1);
}

int Launchers::write_text_file( LPSTR name, LPSTR txt )
{
	int ret = 0;
	CFile f;
	if(name != "" && do_create_file( &f, name )) {
		TRY
		{
			char *crlf = "\r\n";
			f.Write( txt, strlen(txt) );
			f.Write( crlf, strlen(crlf) );
			ret = 1;
		}
		CATCH( CFileException, e )
		{
		}
		END_CATCH
		silent_close( &f );
	}
	return(ret);
}

/*
"My Document.lnk"
"My Document Win32.lnk"
"My Document (Win32).lnk"
"Win32 - My Document.lnk"
*/
void Launchers::add_link_tail( char *lpszPathLink, char *id )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	char basename[MAX_PATH];

	strcpy( basename, lpszPathLink );
	switch( settings_.m_link_name_style ) {
		case 0:
			break;
		case 1:
			sprintf( lpszPathLink, "%s %s", basename, id );
			break;
		case 2:
			sprintf( lpszPathLink, "%s (%s)", basename, id );
			break;
		case 3:
			sprintf( lpszPathLink, "%s - %s", id, basename );
			break;
	}
}

void make_short_name( CString &path )
{
	char *p, short_name[_MAX_PATH];

	p = path.GetBuffer( _MAX_PATH );
	*short_name = 0;
	GetShortPathName( p, short_name, _MAX_PATH );
	if(*short_name) lstrcpy( p, short_name );
	path.ReleaseBuffer();
}

int Launchers::do_make_shortcut_e_dos( CString application, CString creator, int make_shortcut )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString document;
	CString CmdLine;
	char *p;

	char lpszPathObj[MAX_PATH];
  char lpszPathLink[MAX_PATH];
	char lpszDesc[MAX_PATH];
	char lpszIconFile[MAX_PATH];
	char lpszWorkingDirectory[MAX_PATH];
	char param_file_name[MAX_PATH];
	int showCmd, retval = 0;
	int is_doc = (application != "");

	if(!LegacyDocument(document)) return(0);

	const auto invocation=SplitExecutorCommand(settings_.m_executor_dos_path);
	if(invocation.first.IsEmpty()||strcpy_s(lpszPathObj,invocation.first.GetString()))return 0;

	if(make_shortcut) {
		make_shortcut_name( document, lpszPathLink );
		add_link_tail( lpszPathLink, "DOS" );
		strcat( lpszPathLink, ".lnk" );
		make_shortcut_name( document, lpszDesc );
		make_icon_name( lpszIconFile );
		make_e_dos_param_file_name( param_file_name );
		if(!SaveFileIcon(request_.small,request_.large,lpszIconFile,is_doc)) {
			CString cs = "Failed to save the icon to file \"" + CString(lpszIconFile) + ".\"";
			AfxMessageBox( cs );
			*lpszIconFile = 0;
		}
	} else {
		GetModuleFileName( pApp->m_hInstance, param_file_name, _MAX_PATH );
		p = strrchr( param_file_name, '\\' );
		if(p) *p = 0;
		strcat( param_file_name, "\\TmpParam.txt" );
	}

	strcpy( lpszWorkingDirectory, lpszPathObj );
	p = strrchr( lpszWorkingDirectory, '\\' );
	if(p) *p = 0;
	showCmd = SW_SHOWMAXIMIZED;

	CmdLine=ExecutorArguments(invocation.second,document,application,true);
	LPSTR args = CmdLine.GetBuffer( MAX_PATH );
	write_text_file( param_file_name, args );
	CmdLine.ReleaseBuffer();

	char param_string[MAX_PATH];
	*param_string = '@';
	GetShortPathName( param_file_name, &param_string[1], MAX_PATH-1 );

	if(make_shortcut) {
		HRESULT hs = CreateLinkDesktop(
			session_.Owner()->GetSafeHwnd(),
			lpszPathObj,
			lpszPathLink,
			lpszDesc,
			lpszIconFile,
			lpszWorkingDirectory,
			param_string,
			showCmd,
			TRUE
		);
		retval = (hs == 0);
	} else {
		HINSTANCE hinst = ShellExecute(
			NULL,
			"open",
			lpszPathObj,
			param_string,
			lpszWorkingDirectory,
			showCmd
		);
		retval = (hinst > (HINSTANCE)32);
	}

	return(retval);
}

int Launchers::do_make_shortcut_e_win32( CString application, CString creator, int make_shortcut )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString document;
	CString CmdLine;
	char *p;

	char lpszPathObj[MAX_PATH];
  char lpszPathLink[MAX_PATH];
	char lpszDesc[MAX_PATH];
	char lpszIconFile[MAX_PATH];
	char lpszWorkingDirectory[MAX_PATH];
	int showCmd, retval = 0;
	int is_doc = (application != "");

	if(!LegacyDocument(document)) return(0);

	const auto invocation=SplitExecutorCommand(settings_.m_executor_win32_path);
	if(invocation.first.IsEmpty()||strcpy_s(lpszPathObj,invocation.first.GetString()))return 0;

	if(make_shortcut) {
		make_shortcut_name( document, lpszPathLink );
		add_link_tail( lpszPathLink, "Win32" );
		strcat( lpszPathLink, ".lnk" );
		make_shortcut_name( document, lpszDesc );
		make_icon_name( lpszIconFile );
		if(!SaveFileIcon(request_.small,request_.large,lpszIconFile,is_doc)) {
			CString cs = "Failed to save the icon to file \"" + CString(lpszIconFile) + ".\"";
			AfxMessageBox( cs );
			*lpszIconFile = 0;
		}
	}

	strcpy( lpszWorkingDirectory, lpszPathObj );
	p = strrchr( lpszWorkingDirectory, '\\' );
	if(p) *p = 0;
	showCmd = SW_SHOWNORMAL;

	CmdLine=ExecutorArguments(invocation.second,document,application,false);
	LPSTR args = CmdLine.GetBuffer( MAX_PATH );
	if(make_shortcut) {
		HRESULT hs = CreateLinkDesktop(
			session_.Owner()->GetSafeHwnd(),
			lpszPathObj,
			lpszPathLink,
			lpszDesc,
			lpszIconFile,
			lpszWorkingDirectory,
			args,
			showCmd,
			TRUE
		);
		retval = (hs == 0);
	} else {
		HINSTANCE hinst = ShellExecute(
			NULL,
			"open",
			lpszPathObj,
			args,
			lpszWorkingDirectory,
			showCmd
		);
		retval = (hinst > (HINSTANCE)31);
		if(retval) {
			static_cast<CMainFrame*>(session_.Owner())->WaitEmulator( "Exewin32", "exec_win32" );
		}
	}
	CmdLine.ReleaseBuffer();

	return(retval);
}

int Launchers::do_make_shortcut_windows( CString type, CString creator, int make_shortcut )
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

int Launchers::do_make_shortcut_vmac( CString application, CString creator, int make_shortcut )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString document;
	CString CmdLine;
	char *p;

	char lpszPathObj[MAX_PATH];
  char lpszPathLink[MAX_PATH];
	char lpszDesc[MAX_PATH];
	char lpszIconFile[MAX_PATH];
	char lpszWorkingDirectory[MAX_PATH];
	int showCmd;
	int is_doc = (application == "D:"), retval = 0;

	if(settings_.m_launch_method == 0) {
		application = CString("S:");
	}

	if(!LegacyDocument(document)) return(0);

	get_vmac_launcher_name( lpszPathObj );

	if(make_shortcut) {
		make_shortcut_name( document, lpszPathLink );
		add_link_tail( lpszPathLink, "vMac" );
		strcat( lpszPathLink, ".lnk" );
		make_shortcut_name( document, lpszDesc );
		make_icon_name( lpszIconFile );
		if(!SaveFileIcon(request_.small,request_.large,lpszIconFile,is_doc)) {
			CString cs = "Failed to save the icon to file \"" + CString(lpszIconFile) + ".\"";
			AfxMessageBox( cs );
			*lpszIconFile = 0;
		}
	}

	strcpy( lpszWorkingDirectory, lpszPathObj );
	p = strrchr( lpszWorkingDirectory, '\\' );
	if(p) *p = 0;
	showCmd = SW_SHOWMAXIMIZED;

	if(is_mac_path(document)) {
		CmdLine =
						DiskArguments(request_.selection,application);
	} else {
		AfxMessageBox( "Objects on FAT volumes cannot be launched to vMac. Please select a target that is located on a DSK or HFV volume file." );
		return(0);
	}

	LPSTR args = CmdLine.GetBuffer( MAX_PATH );
	if(make_shortcut) {
		HRESULT hs = CreateLinkDesktop(
			session_.Owner()->GetSafeHwnd(),
			lpszPathObj,
			lpszPathLink,
			lpszDesc,
			lpszIconFile,
			lpszWorkingDirectory,
			args,
			showCmd,
			TRUE
		);
		retval = (hs == 0);
	} else {
		HINSTANCE hinst = ShellExecute(
			NULL,
			"open",
			lpszPathObj,
			args,
			lpszWorkingDirectory,
			showCmd
		);

		retval = (hinst > (HINSTANCE)31);
		if(retval) {
			static_cast<CMainFrame*>(session_.Owner())->WaitEmulator( "vMac", "vMac Win32" );
		}
		/*
		if(retval) {
			if(pApp->GetProfileInt("Setup","TerminateWarningIssued",0) == 0) {
				pApp->WriteProfileInt("Setup","TerminateWarningIssued",1);
				CString cs = "For your safety, HFVExplorer will now quit.";
				AfxMessageBox( cs );
			}
			session_.Owner()->PostMessage(WM_CLOSE);
		}
		*/
	}
	CmdLine.ReleaseBuffer();
	return(retval);
}

int Launchers::do_make_shortcut_ss( CString application, CString creator, int make_shortcut )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString document;
	CString CmdLine;
	char *p;

	char lpszPathObj[MAX_PATH];
  char lpszPathLink[MAX_PATH];
	char lpszDesc[MAX_PATH];
	char lpszIconFile[MAX_PATH];
	char lpszWorkingDirectory[MAX_PATH];
	int showCmd;
	int is_doc = (application == "D:"), retval = 0;

	if(settings_.m_launch_method == 0) {
		application = CString("S:");
	}

	if(!LegacyDocument(document)) return(0);

	get_vmac_launcher_name( lpszPathObj );

	if(make_shortcut) {
		make_shortcut_name( document, lpszPathLink );
		add_link_tail( lpszPathLink, "ShapeShifter" );
		strcat( lpszPathLink, ".lnk" );
		make_shortcut_name( document, lpszDesc );
		make_icon_name( lpszIconFile );
		if(!SaveFileIcon(request_.small,request_.large,lpszIconFile,is_doc)) {
			CString cs = "Failed to save the icon to file \"" + CString(lpszIconFile) + ".\"";
			AfxMessageBox( cs );
			*lpszIconFile = 0;
		}
	}

	strcpy( lpszWorkingDirectory, lpszPathObj );
	p = strrchr( lpszWorkingDirectory, '\\' );
	if(p) *p = 0;
	showCmd = SW_SHOWMAXIMIZED;

	if(is_mac_path(document)) {
		CmdLine =
						CString("-SS ") +
						DiskArguments(request_.selection,application);
	} else {
		AfxMessageBox( "Objects on FAT volumes cannot be launched to ShapeShifter. Please select a target that is located on a DSK or HFV volume file." );
		return(0);
	}

	LPSTR args = CmdLine.GetBuffer( MAX_PATH );
	if(make_shortcut) {
		HRESULT hs = CreateLinkDesktop(
			session_.Owner()->GetSafeHwnd(),
			lpszPathObj,
			lpszPathLink,
			lpszDesc,
			lpszIconFile,
			lpszWorkingDirectory,
			args,
			showCmd,
			TRUE
		);
		retval = (hs == 0);
	} else {
		HINSTANCE hinst = ShellExecute(
			NULL,
			"open",
			lpszPathObj,
			args,
			lpszWorkingDirectory,
			showCmd
		);

		retval = (hinst > (HINSTANCE)31);
		if(retval) {
			// static_cast<CMainFrame*>(session_.Owner())->WaitEmulator( "", "" );
		}
	}
	CmdLine.ReleaseBuffer();
	return(retval);
}

int Launchers::do_make_shortcut_fusion( CString application, CString creator, int make_shortcut )
{
	CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
	CString document;
	CString CmdLine;
	char *p;

	char lpszPathObj[MAX_PATH];
  char lpszPathLink[MAX_PATH];
	char lpszDesc[MAX_PATH];
	char lpszIconFile[MAX_PATH];
	char lpszWorkingDirectory[MAX_PATH];
	int showCmd;
	int is_doc = (application == "D:"), retval = 0;

	if(settings_.m_launch_method == 0) {
		application = CString("S:");
	}

	if(!LegacyDocument(document)) return(0);

	get_vmac_launcher_name( lpszPathObj );

	if(make_shortcut) {
		make_shortcut_name( document, lpszPathLink );
		add_link_tail( lpszPathLink, "Fusion" );
		strcat( lpszPathLink, ".lnk" );
		make_shortcut_name( document, lpszDesc );
		make_icon_name( lpszIconFile );
		if(!SaveFileIcon(request_.small,request_.large,lpszIconFile,is_doc)) {
			CString cs = "Failed to save the icon to file \"" + CString(lpszIconFile) + ".\"";
			AfxMessageBox( cs );
			*lpszIconFile = 0;
		}
	}

	strcpy( lpszWorkingDirectory, lpszPathObj );
	p = strrchr( lpszWorkingDirectory, '\\' );
	if(p) *p = 0;
	showCmd = SW_SHOWMAXIMIZED;

	if(is_mac_path(document)) {
		CmdLine =
						CString("-FU ") +
						DiskArguments(request_.selection,application);
	} else {
		AfxMessageBox( "Objects on FAT volumes cannot be launched to Fusion. Please select a target that is located on a DSK or HFV volume file." );
		return(0);
	}

	LPSTR args = CmdLine.GetBuffer( MAX_PATH );
	if(make_shortcut) {
		HRESULT hs = CreateLinkDesktop(
			session_.Owner()->GetSafeHwnd(),
			lpszPathObj,
			lpszPathLink,
			lpszDesc,
			lpszIconFile,
			lpszWorkingDirectory,
			args,
			showCmd,
			TRUE
		);
		retval = (hs == 0);
	} else {
		HINSTANCE hinst = ShellExecute(
			NULL,
			"open",
			lpszPathObj,
			args,
			lpszWorkingDirectory,
			showCmd
		);
		retval = (hinst > (HINSTANCE)31);
		if(retval) {
			// static_cast<CMainFrame*>(session_.Owner())->WaitEmulator( "", "" );
		}
	}
	CmdLine.ReleaseBuffer();
	return(retval);
}

int Launchers::do_make_shortcut( CString application, CString creator, Target emulator, int make_shortcut )
{
	int ret = 0;

	if(!check_shortcut_installation(emulator)) {
		AfxMessageBox( "Please check the program locations and parameters in Options dialog before making shortcuts." );
		return(0);
	}

	switch( emulator ) {
		case Target::ExecutorDos:
			ret = do_make_shortcut_e_dos(application,creator,make_shortcut);
			break;
		case Target::ExecutorWin32:
			ret = do_make_shortcut_e_win32(application,creator,make_shortcut);
			break;
		case Target::VMac:
			ret = do_make_shortcut_vmac(application,creator,make_shortcut);
			break;
		case Target::ShapeShifter:
			ret = do_make_shortcut_ss(application,creator,make_shortcut);
			break;
		case Target::Fusion:
			ret = do_make_shortcut_fusion(application,creator,make_shortcut);
			break;
		case Target::Windows:
			ret = do_make_shortcut_windows(application,creator,make_shortcut);
			break;
	}
	if(make_shortcut) {
		if(ret) {
			AfxMessageBox( "A shortcut was placed on your desktop." );
		} else {
			AfxMessageBox( "Failed to create a shortcut." );
		}
	}
	return ret;
}

bool Launchers::Run()
{
 StorageLocation::RequireCapturedIdentity(request_.selection);
 char typeBytes[5]{},creatorBytes[5]{};
 memcpy(typeBytes,request_.entry.finder.data(),4);memcpy(creatorBytes,request_.entry.finder.data()+4,4);
 CString type(typeBytes),creator(creatorBytes),program;
 auto target=request_.target;
 if(request_.selection.IsWindows()&&type.IsEmpty()){
  if(request_.shortcut){AfxMessageBox("Please make Windows shortcuts in Explorer.");return false;}
  return OpenWindows(session_.Owner(),request_.selection,nullptr);
 }
 if(target==Target::Default&&request_.selection.IsClassic()){
  static constexpr Target defaults[]={Target::ExecutorDos,Target::ExecutorWin32,Target::VMac,
      Target::ShapeShifter,Target::Fusion,Target::Windows};
  const auto choice=settings_.defaultEmulator;
  target=choice>=0&&choice<int(ARRAYSIZE(defaults))?defaults[choice]:Target::ExecutorWin32;
 }
 if(target==Target::VMac||target==Target::ShapeShifter){
  if(request_.selection.IsWindows()){
   AfxMessageBox("vMac and ShapeShifter shortcuts cannot refer to DOS volumes.");
   return false;
  }
  program=type=="APPL"?"A:":"D:";
 }else if(target==Target::Windows&&request_.selection.IsClassic()){
  if(type=="APPL"){
   AfxMessageBox("You cannot launch Mac applications with Windows programs, only documents.");
   return false;
  }
  program=type;
 }else if(type!="APPL"){
  program=session_.Document()->FindClassicLauncher("APPL",creator,request_.classicSlot);
  if(program.IsEmpty()){
   if(type=="TEXT"||type=="ttro"||type=="ttxt")program="System:Shareware:Tex-Edit:Tex-Edit";
   else{
    AfxMessageBox("Could not find association for "+CString(MacBrowser::Wide(request_.entry.name).c_str()));
    return false;
   }
  }
 }
 if(target==Target::Default)return RunExecutor(session_.Owner(),settings_.m_executor_win32_path,request_.selection,program);
 return do_make_shortcut(program,creator,target,request_.shortcut)!=0;
}
}

Target FromMenuCommand(int command)
{
 switch(command){
 case ID_OWN_SHORTCUT_CREATE_E_DOS:return Target::ExecutorDos;
 case ID_OWN_SHORTCUT_CREATE_E_WIN32:return Target::ExecutorWin32;
 case ID_OWN_SHORTCUT_CREATE_VMAC:return Target::VMac;
 case ID_OWN_SHORTCUT_CREATE_SS:return Target::ShapeShifter;
 case ID_OWN_SHORTCUT_CREATE_FUSION:return Target::Fusion;
 case ID_OWN_SHORTCUT_WINDOWS:return Target::Windows;
 default:return Target::Default;
 }
}

bool Launch(BrowserSession& session,Request request)
{
 try{return Launchers(session,request).Run();}
 catch(const std::exception& error){session.Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);return false;}
}

bool OpenWith(BrowserSession& session,const Location& source,const Location& application,bool mac)
{
 try{
  if(!mac)return OpenWindows(session.Owner(),source,&application);
  if(source.IsClassic()&&!VolumeCoord::Readable(source.expectedResource))return false;
  const Settings settings(*static_cast<CHFVExplorerApp*>(AfxGetApp()));
  return RunExecutor(session.Owner(),settings.m_executor_win32_path,source,application.LegacyPath().c_str());
 }catch(const std::exception& error){
  session.Owner()->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }
}

bool SaveFileIcon(HICON small,HICON large,const char* path,bool document)
{
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 if(!small)small=large?large:app.LoadIcon(document?IDI_ICON9:IDI_ICON5);
 if(!large)large=small;
 return small&&large&&SaveIcon(small,large,const_cast<char*>(path))!=0;
}

CString ExecutorCommand(const CString& configured,const Location& source,CString application)
{
 return ExecutorArguments(configured,source.LegacyPath().c_str(),std::move(application),false);
}

CString DiskArguments(const Location& source,const CString& operation)
{
 if(!source.IsClassic())throw std::invalid_argument("An emulator disk argument requires a classic HFS volume");
 return CString(source.ClassicImage().c_str())+"?"+operation+make_mac_chrset(source.ClassicPath().c_str());
}

std::wstring WindowsArgument(const Location& source)
{
 return L"\""+source.WindowsPath()+L"\"";
}
}
