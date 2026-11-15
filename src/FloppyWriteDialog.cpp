// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "FloppyWriter.h"
#include "FloppyWriteDialog.h"
#include "charset.h"
#include <thread>
#include <exception>

namespace {
const UINT WM_FLOPPY_PROGRESS=WM_APP+80;
struct FloppyProgressUpdate {const char* phase;unsigned completed,total;};
}

// Diagnostics belong on a fixed host volume, never on the target floppy.
static CString SaveFloppyDiagnostics(const FloppyWriter& writer) {
 char folder[MAX_PATH],root[MAX_PATH],path[MAX_PATH];
 if(!GetTempPathA(MAX_PATH,folder) || !GetVolumePathNameA(folder,root,MAX_PATH) || GetDriveTypeA(root)!=DRIVE_FIXED)
  return "Log unavailable: no fixed-volume temporary directory.";
 if(!GetTempFileNameA(folder,"hfv",0,path)) return "Log unavailable: could not create temporary file.";
 HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
 if(file==INVALID_HANDLE_VALUE) return "Log unavailable: could not open temporary file.";
 const std::string text=std::string("HFVExplorer floppy diagnostics v2\r\n")+writer.Diagnostics();DWORD wrote=0;
 bool ok=WriteFile(file,text.data(),DWORD(text.size()),&wrote,NULL)&&wrote==text.size();CloseHandle(file);
 return ok?CString(path):CString("Log unavailable: could not write complete report.");
}

class FloppyWriteDialog : public CDialog {
public:
 FloppyWriteDialog(CWnd* parent,void(*release)(int,void*),void* context):CDialog(IDD_FLOPPY_WRITE,parent),release_(release),context_(context) {INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);}
protected:
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();CComboBox* combo=(CComboBox*)GetDlgItem(IDC_FW_DRIVE);
  for(int drive=0;drive<26;++drive) {char root[]="A:\\";root[0]+=char(drive);if(GetDriveTypeA(root)==DRIVE_REMOVABLE) {CString text;text.Format("%c:",'A'+drive);int i=combo->AddString(text);combo->SetItemData(i,drive);}}
  if(combo->GetCount()) combo->SetCurSel(0);SetDlgItemText(IDC_FW_LABEL,"Macintosh");return TRUE;
 }
 afx_msg void Browse() {CFileDialog dialog(TRUE,"dsk",NULL,OFN_FILEMUSTEXIST|OFN_HIDEREADONLY,"Raw HFS images|*.dsk;*.hfv;*.img|All files|*.*||",this);if(dialog.DoModal()==IDOK)SetDlgItemText(IDC_FW_IMAGE,dialog.GetPathName());}
 afx_msg void Format() {Run(true);}
 afx_msg void Write() {Run(false);}
 void OnCancel() override {if(!busy_)CDialog::OnCancel();}
 void OnOK() override {if(!busy_)CDialog::OnOK();}
 afx_msg void OnClose() {if(!busy_)CDialog::OnClose();}
 afx_msg LRESULT OnProgress(WPARAM,LPARAM data) {
  const auto& update=*reinterpret_cast<const FloppyProgressUpdate*>(data);
  // Routine readiness checks stay in the diagnostic trace. Showing them for
  // every track makes the visible phase alternate several times per transfer.
  if(!strcmp(update.phase,"Checking media before write") || !strcmp(update.phase,"Checking media before readback"))return 0;
  bool waiting=!strcmp(update.phase,"Formatting tracks") || !strcmp(update.phase,"Flushing written data");
  CProgressCtrl* bar=(CProgressCtrl*)GetDlgItem(IDC_FW_PROGRESS);
  bool marquee=(bar->GetStyle()&PBS_MARQUEE)!=0;
  if(waiting && !marquee) {bar->ModifyStyle(0,PBS_MARQUEE);bar->SendMessage(PBM_SETMARQUEE,TRUE,40);}
  else if(!waiting) {
   if(marquee) {bar->SendMessage(PBM_SETMARQUEE,FALSE,0);bar->ModifyStyle(PBS_MARQUEE,0);}
   if(bar->GetPos()!=int(update.completed))bar->SetPos(update.completed);
  }
  CString status;
  if(waiting)status.Format("%s...",update.phase);
  else status.Format("%s: %u / %u tracks (%u%%)",update.phase,update.completed,update.total,update.total?unsigned(100ULL*update.completed/update.total):0);
  CString previous;GetDlgItemText(IDC_FW_STATUS,previous);
  if(previous!=status)SetDlgItemText(IDC_FW_STATUS,status);return 0;
 }
 DECLARE_MESSAGE_MAP()
private:
 bool busy_=false;void(*release_)(int,void*);void* context_;
 static void Progress(const char* phase,unsigned completed,unsigned total,void* context) {
  // SendMessage keeps the stack payload alive until the UI thread consumes it.
  FloppyProgressUpdate update={phase,completed,total};
  ::SendMessage((HWND)context,WM_FLOPPY_PROGRESS,0,reinterpret_cast<LPARAM>(&update));
 }
 void Run(bool format) {
  if(busy_) return;
  CComboBox* combo=(CComboBox*)GetDlgItem(IDC_FW_DRIVE);int selected=combo->GetCurSel();
  if(selected==CB_ERR) {AfxMessageBox("Select a floppy drive.");return;}
  int drive=(int)combo->GetItemData(selected);CString path,label;GetDlgItemText(IDC_FW_IMAGE,path);GetDlgItemText(IDC_FW_LABEL,label);
  std::vector<BYTE> image;std::string error;
  if(format) {
   char name[256];strncpy_s(name,(LPCSTR)label,_TRUNCATE);pc_to_mac_charset((unsigned char*)name);
   if(!PrepareFloppyHfs(name,image,error)) {AfxMessageBox(error.c_str());return;}
  } else if(!LoadFloppyHfs(path,image,error)) {AfxMessageBox(error.c_str());return;}
  bool tracks=IsDlgButtonChecked(IDC_FW_TRACKS)==BST_CHECKED;CString question;
  question.Format("ERASE ALL DATA on floppy drive %c:?\r\n\r\n%s\r\n%s\r\n\r\nSize: 1,474,560 bytes (1.44 MB). The result will be read back and verified. Other drive types are rejected.",
      'A'+drive,format?(LPCSTR)(CString("Create classic HFS volume: ")+label):(LPCSTR)(CString("Write image: ")+path),tracks?"Low-level track formatting is also requested.":"Existing sector formatting will be used.");
  if(AfxMessageBox(question,MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
  if(release_)release_(drive,context_);
  busy_=true;for(int id:{IDC_FW_FORMAT,IDC_FW_WRITE,IDC_FW_BROWSE,IDC_FW_DRIVE,IDC_FW_IMAGE,IDC_FW_LABEL,IDC_FW_TRACKS,IDCANCEL})GetDlgItem(id)->EnableWindow(FALSE);
  CProgressCtrl* bar=(CProgressCtrl*)GetDlgItem(IDC_FW_PROGRESS);bar->SendMessage(PBM_SETMARQUEE,FALSE,0);bar->ModifyStyle(PBS_MARQUEE,0);bar->SetRange32(0,160);bar->SetPos(0);bar->SendMessage(PBM_SETSTATE,PBST_NORMAL,0);
  SetDlgItemText(IDC_FW_STATUS,"Preparing drive...");
  FloppyWriter writer;bool ok=false;CString workerError;
  {
   CWaitCursor wait;
   try {
    HWND window=GetSafeHwnd();
    std::thread worker([&] {
     try {ok=writer.WriteImage(drive,image,tracks,Progress,window);}
     catch(const std::exception& error) {workerError=error.what();}
     catch(...) {workerError="Unexpected worker failure.";}
    });
    // Pump the modal dialog while the worker owns the device. Controls and close
    // actions stay disabled until the transaction has finished and joined.
    HANDLE thread=worker.native_handle();bool quit=false;int quitCode=0;
    while(MsgWaitForMultipleObjects(1,&thread,FALSE,INFINITE,QS_ALLINPUT)!=WAIT_OBJECT_0) {
     MSG message;
     while(PeekMessage(&message,NULL,0,0,PM_REMOVE)) {
      if(message.message==WM_QUIT) {quit=true;quitCode=(int)message.wParam;continue;}
      if(!AfxGetApp()->PreTranslateMessage(&message)) {TranslateMessage(&message);DispatchMessage(&message);}
     }
    }
    worker.join();if(quit)PostQuitMessage(quitCode);
   } catch(const std::exception& error) {workerError=error.what();}
  }
  busy_=false;for(int id:{IDC_FW_FORMAT,IDC_FW_WRITE,IDC_FW_BROWSE,IDC_FW_DRIVE,IDC_FW_IMAGE,IDC_FW_LABEL,IDC_FW_TRACKS,IDCANCEL})GetDlgItem(id)->EnableWindow(TRUE);
  bar->SendMessage(PBM_SETMARQUEE,FALSE,0);bar->ModifyStyle(PBS_MARQUEE,0);bar->SetRange32(0,160);bar->SetPos(writer.VerifiedTracks()?writer.VerifiedTracks():writer.WrittenTracks());
  bar->SendMessage(PBM_SETSTATE,ok?PBST_NORMAL:PBST_ERROR,0);
  CString log=SaveFloppyDiagnostics(writer);
  if(!workerError.IsEmpty()) {CString message;message.Format("Floppy worker failed: %s\r\nThe disk may be incomplete.\r\nDiagnostic log: %s",(LPCSTR)workerError,(LPCSTR)log);SetDlgItemText(IDC_FW_STATUS,"Worker failed; see details.");AfxMessageBox(message,MB_ICONERROR|MB_OK);return;}
  if(ok) {SetDlgItemText(IDC_FW_STATUS,"Complete: all 1,474,560 bytes verified.");CString message;message.Format("The HFS floppy was written and verified. Close this window to refresh the volume list.\r\n\r\nDiagnostic log: %s",(LPCSTR)log);if(writer.FlushError())message+="\r\n\r\nThe drive does not support an explicit flush request. All sectors were written and verified using unbuffered, write-through access.";AfxMessageBox(message,MB_OK|MB_ICONINFORMATION);}
  else {
   char system[512]={};FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,writer.Error(),0,system,sizeof(system),NULL);
   CString message;message.Format("Floppy operation failed (Windows error %lu). %s\r\nStage: %s\r\nByte offset: %lu; sector (zero-based): %lu\r\nCompleted: %u / 160 writes; %u / 160 readbacks\r\n\r\n%s\r\n\r\nDiagnostic log: %s",writer.Error(),system,writer.Stage().c_str(),writer.Offset(),writer.Offset()/512,writer.WrittenTracks(),writer.VerifiedTracks(),writer.WritesStarted()?"Writing/formatting started; the disc may be incomplete. Do not treat it as a successful format.":"No data writes or track-format commands were issued.",(LPCSTR)log);
   if(writer.FlushError()) {
    CString flush;flush.Format("\r\n\r\nFlush error: %lu. %s",writer.FlushError(),writer.VerifiedTracks()==160?"All sectors read back identically, but the flush failed; durability is not confirmed.":"Readback verification also did not complete.");message+=flush;
   }
   SetDlgItemText(IDC_FW_STATUS,"Failed; see stage, sector and diagnostic log.");AfxMessageBox(message,MB_ICONERROR|MB_OK);
  }
 }
};
BEGIN_MESSAGE_MAP(FloppyWriteDialog,CDialog)
 ON_WM_CLOSE()
 ON_MESSAGE(WM_FLOPPY_PROGRESS,OnProgress)
 ON_BN_CLICKED(IDC_FW_BROWSE,Browse)
 ON_BN_CLICKED(IDC_FW_FORMAT,Format)
 ON_BN_CLICKED(IDC_FW_WRITE,Write)
END_MESSAGE_MAP()
void ShowFloppyWriteDialog(CWnd* parent,void(*release)(int,void*),void* context) {FloppyWriteDialog dialog(parent,release,context);dialog.DoModal();}
