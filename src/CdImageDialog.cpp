// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "CdImageDialog.h"
#include "CdImage.h"
#include "StorageJobDialog.h"
namespace {
class CdImageDialog : public CDialog {
public:
 int drive=-1;CString output;
 explicit CdImageDialog(CWnd* owner):CDialog(IDD_CD_IMAGE_SOURCE,owner){}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();auto combo=(CComboBox*)GetDlgItem(IDC_CDI_DRIVE);
  for(int n=0;n<26;++n){char root[]="A:\\";root[0]+=char(n);if(GetDriveTypeA(root)==DRIVE_CDROM){int item=combo->AddString(root);combo->SetItemData(item,n);}}
  if(combo->GetCount())combo->SetCurSel(0);return TRUE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)==IDC_CDI_BROWSE){
   CFileDialog save(FALSE,"iso",nullptr,OFN_HIDEREADONLY|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR,"Data CD images (*.iso)|*.iso|Raw images (*.img)|*.img|All files (*.*)|*.*||",this);
   if(save.DoModal()==IDOK)SetDlgItemText(IDC_CDI_PATH,save.GetPathName());return TRUE;
  }
  return CDialog::OnCommand(w,l);
 }
 void OnOK() override {
  auto combo=(CComboBox*)GetDlgItem(IDC_CDI_DRIVE);int item=combo->GetCurSel();
  GetDlgItemText(IDC_CDI_PATH,output);output.TrimLeft();output.TrimRight();
  if(item==CB_ERR){AfxMessageBox("Select a CD drive.");return;}
  if(output.IsEmpty()){AfxMessageBox("Choose an image file to save.");return;}
  drive=int(combo->GetItemData(item));CDialog::OnOK();
 }
};
}
void ShowCdImageDialog(CWnd* owner) {
 CdImageDialog options(owner);if(options.DoModal()!=IDOK)return;
 auto result=ShowStorageJob(MakeCdImageJob(options.drive,(LPCSTR)options.output),"Saving CD image",false,true,true);
 if(result.code&&result.error!=ERROR_CANCELLED)AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
}
