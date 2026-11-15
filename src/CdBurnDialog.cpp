// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "CdBurn.h"
#include "CdBurnDialog.h"
#include "StorageJobDialog.h"
namespace {
class BurnDialog : public CDialog {
 const std::vector<CdRecorder>& recorders_;
public:
 int selected=-1;CString image;
 BurnDialog(CWnd* owner,const std::vector<CdRecorder>& recorders,const char* initial):CDialog(IDD_CD_BURN,owner),recorders_(recorders),image(initial){}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();auto combo=(CComboBox*)GetDlgItem(IDC_CDB_RECORDER);
  for(const auto& recorder:recorders_)combo->AddString(recorder.name.c_str());if(combo->GetCount())combo->SetCurSel(0);
  SetDlgItemText(IDC_CDB_IMAGE,image);return TRUE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)==IDC_CDB_BROWSE){
   CFileDialog source(TRUE,"iso",nullptr,OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR,"Raw data images|*.iso;*.img;*.dsk;*.hfv|All files|*.*||",this);
   if(source.DoModal()==IDOK)SetDlgItemText(IDC_CDB_IMAGE,source.GetPathName());return TRUE;
  }return CDialog::OnCommand(w,l);
 }
 void OnOK() override {
  selected=((CComboBox*)GetDlgItem(IDC_CDB_RECORDER))->GetCurSel();GetDlgItemText(IDC_CDB_IMAGE,image);image.TrimLeft();image.TrimRight();
  if(selected==CB_ERR){AfxMessageBox("Select a recorder.");return;}if(image.IsEmpty()){AfxMessageBox("Select a raw data image.");return;}CDialog::OnOK();
 }
};
}
void ShowCdBurnDialog(CWnd* owner,const char* initialImage){
 auto recorders=std::make_shared<std::vector<CdRecorder>>();auto result=ShowStorageWaitJob(ListCdRecorders(recorders));
 if(result.code){AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return;}
 if(recorders->empty()){AfxMessageBox("Windows IMAPI2 did not report any usable optical recorders.");return;}
 BurnDialog dialog(owner,*recorders,initialImage);if(dialog.DoModal()!=IDOK)return;
 const auto recorder=(*recorders)[dialog.selected];auto plan=std::make_shared<CdBurnPlan>();
 result=ShowStorageWaitJob(PrepareCdBurn((LPCSTR)dialog.image,recorder.id,plan));
 if(result.code){AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);return;}
 CString question;question.Format("Burn this image to the blank disc in:\r\n%s\r\n\r\nImage: %s\r\nSize: %.2f MiB\r\n\r\nThe disc will be finalized and fully verified. Existing discs are not erased or overwritten.\r\n\r\nStart burning?",recorder.name.c_str(),(LPCSTR)dialog.image,double(plan->bytes)/(1024*1024));
 if(AfxMessageBox(question,MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
 result=ShowStorageJob(MakeCdBurnJob(*plan),"Burning CD image",false,true,true);
 if(result.code){if(result.error!=ERROR_CANCELLED)AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);else AfxMessageBox("Burn cancelled. If recording started, the disc may be incomplete.",MB_OK|MB_ICONINFORMATION);}
 else AfxMessageBox("CD image burned, finalized and fully verified.",MB_OK|MB_ICONINFORMATION);
}
