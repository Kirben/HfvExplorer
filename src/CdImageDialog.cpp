// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "CdImageDialog.h"
#include "CdImage.h"
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
namespace {
class CdTrackDialog:public CDialog {
public:
 std::vector<CdMedia::Track> tracks;unsigned selected=0;bool macOnly=false;
 CdTrackDialog(CWnd* owner,std::vector<CdMedia::Track> values):CDialog(IDD_CD_DATA_TRACK,owner),tracks(std::move(values)){}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();for(const auto& t:tracks){auto label="Session "+std::to_string(t.session)+", track "+std::to_string(t.number)+" - sector "+std::to_string(t.start)+", "+std::to_string(t.sectors*2048/1024)+" KiB";SendDlgItemMessage(IDC_CDI_TRACK,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}SendDlgItemMessage(IDC_CDI_TRACK,CB_SETCURSEL,tracks.size()-1);return TRUE;}
 bool Capture(){auto index=SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCURSEL);if(index<0||size_t(index)>=tracks.size())return false;selected=unsigned(index);macOnly=IsDlgButtonChecked(IDC_CDI_MAC_ONLY)==BST_CHECKED;return true;}
 void OnOK()override{if(Capture())CDialog::OnOK();}
};
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
   CFileDialog save(FALSE,"iso",nullptr,OFN_HIDEREADONLY|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR,"CD data-track images (*.iso;*.cdr)|*.iso;*.cdr|Raw images (*.img)|*.img|All files (*.*)|*.*||",this);
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
 auto source=MakeNativeCdImageSource();auto tracks=std::make_shared<std::vector<CdMedia::Track>>();auto generation=std::make_shared<DWORD>(0);
 auto inspected=ShowStorageWaitJob(MakeCdTrackInspectionJob(options.drive,source,tracks,generation),false,owner);
 if(inspected.code){if(inspected.error!=ERROR_CANCELLED)AfxMessageBox(inspected.message.c_str(),MB_OK|MB_ICONERROR);return;}
 CdTrackDialog choose(owner,*tracks);if(choose.DoModal()!=IDOK)return;
 CdImageSelection selection;selection.track=tracks->at(choose.selected).number;selection.expected=tracks->at(choose.selected);selection.identityKnown=true;selection.generation=*generation;selection.macVolumeOnly=choose.macOnly;
 BackgroundCopies::StartStorage(MakeCdImageJob(options.drive,(LPCSTR)options.output,source,selection),
     owner,"Saving CD image","CD image dump",{},{true});
}

#ifdef HFV_TESTING
bool TestCdTrackPicker(const std::vector<CdMedia::Track>& tracks){CWnd owner;auto klass=AfxRegisterWndClass(0);if(!owner.CreateEx(0,klass,"Hidden track-picker test",WS_OVERLAPPED,0,0,100,100,nullptr,0))return false;CdTrackDialog dialog(&owner,tracks);bool ok=dialog.Create(IDD_CD_DATA_TRACK,&owner)!=FALSE;
 if(ok){ok=dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCOUNT)==tracks.size()&&dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCURSEL)==tracks.size()-1;dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_SETCURSEL,0);dialog.CheckDlgButton(IDC_CDI_MAC_ONLY,BST_CHECKED);ok=ok&&dialog.Capture()&&dialog.selected==0&&dialog.macOnly;dialog.DestroyWindow();}owner.DestroyWindow();return ok;}
#endif
