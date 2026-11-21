// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "CdImageDialog.h"
#include "CdImage.h"
#include "RawCdImage.h"
#include "BrowserPresentation.h"
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
namespace {
class CdTrackDialog:public CDialog {
public:
 std::vector<CdMedia::Track> tracks;unsigned selected=0;bool macOnly=false,raw=false,session=false;
 CdTrackDialog(CWnd* owner,std::vector<CdMedia::Track> values,bool rawFormat=false,bool sessionPicker=false):CDialog(IDD_CD_DATA_TRACK,owner),tracks(std::move(values)),raw(rawFormat),session(sessionPicker){}
 BOOL OnInitDialog()override{
  CDialog::OnInitDialog();
  if(raw){
   SetWindowText(session?"Choose CD Session":"Choose Raw CD Track");
   SetDlgItemText(IDC_CDI_TRACK_LABEL,session?"Session:":"Track:");
   SetDlgItemText(IDC_CDI_TRACK_SCOPE,"Includes the main channel from INDEX 01. Subchannels are excluded.");
   GetDlgItem(IDC_CDI_MAC_ONLY)->ShowWindow(SW_HIDE);
   SetDlgItemText(IDC_CDI_TRACK_HINT,session?"The BIN/CUE contains the tracks from the selected session.":"Save the selected track without converting its filesystem.");
  }
  for(const auto& t:tracks){
   auto label="Session "+std::to_string(t.session);
   if(!session)label+=", track "+std::to_string(t.number)+(t.data?" (data)":" (audio)")+
      " - sector "+std::to_string(t.start)+", "+std::to_string(t.sectors)+" sectors";
   SendDlgItemMessage(IDC_CDI_TRACK,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
  }
  SendDlgItemMessage(IDC_CDI_TRACK,CB_SETCURSEL,tracks.size()-1);
  return TRUE;
 }
 bool Capture(){auto index=SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCURSEL);if(index<0||size_t(index)>=tracks.size())return false;selected=unsigned(index);macOnly=IsDlgButtonChecked(IDC_CDI_MAC_ONLY)==BST_CHECKED;return true;}
 void OnOK()override{if(Capture())CDialog::OnOK();}
};
class CdImageDialog : public CDialog {
 bool detectDrives_;
 void UpdateAudioNote(){
  const auto mode=SendDlgItemMessage(IDC_CDI_FORMAT,CB_GETCURSEL);
  GetDlgItem(IDC_CDI_AUDIO_NOTE)->ShowWindow(mode==1+int(CdMedia::RawOutput::WaveTracks)?SW_SHOW:SW_HIDE);
 }
public:
 int drive=-1,format=0;std::wstring output;
 explicit CdImageDialog(CWnd* owner,bool detectDrives=true):CDialog(IDD_CD_IMAGE_SOURCE,owner),detectDrives_(detectDrives){}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();auto combo=(CComboBox*)GetDlgItem(IDC_CDI_DRIVE);
  for(int n=0;detectDrives_&&n<26;++n){char root[]="A:\\";root[0]+=char(n);if(GetDriveTypeA(root)==DRIVE_CDROM){int item=combo->AddString(root);combo->SetItemData(item,n);}}
  if(combo->GetCount())combo->SetCurSel(0);
  BrowserPresentation::UnicodeEdit(m_hWnd,IDC_CDI_PATH);
  auto formats=static_cast<CComboBox*>(GetDlgItem(IDC_CDI_FORMAT));
  for(const auto* label:{"Data-track image (.iso)","Session image (.bin + .cue)","Raw track, 2352 bytes/sector (.bin)",
                        "Mode 2 Form 2, 2324 bytes/sector (.bin)","Audio tracks (.wav)"})formats->AddString(label);
  formats->SetCurSel(0);
  UpdateAudioNote();
  return TRUE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)==IDC_CDI_FORMAT&&HIWORD(w)==CBN_SELCHANGE){
   UpdateAudioNote();
   return TRUE;
  }
  if(LOWORD(w)==IDC_CDI_BROWSE){
   const auto mode=SendDlgItemMessage(IDC_CDI_FORMAT,CB_GETCURSEL);
   const wchar_t* exts[]={L"iso",L"cue",L"bin",L"bin",L"wav"};
   const wchar_t* filters[]={L"CD data-track image\0*.iso;*.cdr;*.img\0All files\0*.*\0\0",
     L"BIN/CUE sheet\0*.cue\0\0",L"Raw binary track\0*.bin\0\0",L"Mode 2 Form 2 track\0*.bin\0\0",
     L"WAV audio\0*.wav\0\0"};
   if(mode<0||mode>=5)return TRUE;
   wchar_t buffer[32768]{};
   OPENFILENAMEW save{};
   save.lStructSize=sizeof(save);save.hwndOwner=m_hWnd;save.lpstrFile=buffer;
   save.nMaxFile=ARRAYSIZE(buffer);save.lpstrFilter=filters[mode];save.lpstrDefExt=exts[mode];
   save.Flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
   if(GetSaveFileNameW(&save))SetDlgItemTextW(m_hWnd,IDC_CDI_PATH,buffer);
   return TRUE;
  }
  return CDialog::OnCommand(w,l);
 }
 void OnOK() override {
  auto combo=(CComboBox*)GetDlgItem(IDC_CDI_DRIVE);int item=combo->GetCurSel();
  output=BrowserPresentation::Text(::GetDlgItem(m_hWnd,IDC_CDI_PATH));
  format=int(SendDlgItemMessage(IDC_CDI_FORMAT,CB_GETCURSEL));
  if(item==CB_ERR){AfxMessageBox("Select a CD drive.");return;}
  if(output.empty()||format<0||format>=5){AfxMessageBox("Choose an output file and format.");return;}
  drive=int(combo->GetItemData(item));CDialog::OnOK();
 }
};
}
#ifdef HFV_TESTING
bool ProbeCdImageLayout()
{
 CdImageDialog dialog(CWnd::GetDesktopWindow(),false);
 if(!dialog.Create(IDD_CD_IMAGE_SOURCE,CWnd::GetDesktopWindow()))return false;
 bool ok=dialog.SendDlgItemMessage(IDC_CDI_FORMAT,CB_GETCOUNT)==5;
 for(int mode=0;mode<5;++mode){
  dialog.SendDlgItemMessage(IDC_CDI_FORMAT,CB_SETCURSEL,mode);
  dialog.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_CDI_FORMAT,CBN_SELCHANGE));
  const bool visible=(dialog.GetDlgItem(IDC_CDI_AUDIO_NOTE)->GetStyle()&WS_VISIBLE)!=0;
  ok=ok&&visible==(mode==1+int(CdMedia::RawOutput::WaveTracks));
 }
 dialog.DestroyWindow();
 for(const bool raw:{false,true}){
  CdTrackDialog track(CWnd::GetDesktopWindow(),{CdMedia::Track{}},raw);
  if(!track.Create(IDD_CD_DATA_TRACK,CWnd::GetDesktopWindow()))return false;
  const bool choiceVisible=(track.GetDlgItem(IDC_CDI_MAC_ONLY)->GetStyle()&WS_VISIBLE)!=0;
  ok=ok&&choiceVisible==!raw&&track.Capture()&&!track.macOnly&&
      BrowserPresentation::Text(::GetDlgItem(track.m_hWnd,IDOK))==L"Save Image";
  if(!raw){
   track.CheckDlgButton(IDC_CDI_MAC_ONLY,BST_CHECKED);
   ok=ok&&track.Capture()&&track.macOnly;
  }
  track.DestroyWindow();
 }
 return ok;
}
#endif
void ShowCdImageDialog(CWnd* owner) {
 CdImageDialog options(owner);if(options.DoModal()!=IDOK)return;
 auto tracks=std::make_shared<std::vector<CdMedia::Track>>();
 auto generation=std::make_shared<DWORD>(0);
 if(options.format){
  using namespace CdMedia;
  const auto format=static_cast<RawOutput>(options.format-1);
  auto source=NativeRawSource();
  auto inspected=ShowStorageWaitJob(InspectRawCd(options.drive,source,tracks,generation),false,owner);
  if(inspected.code){if(inspected.error!=ERROR_CANCELLED)AfxMessageBox(inspected.message.c_str(),MB_OK|MB_ICONERROR);return;}
  std::vector<Track> selection;
  if(format==RawOutput::WaveTracks){
   for(const auto& track:*tracks)if(!track.data)selection.push_back(track);
  }else if(format==RawOutput::BinCue){
   std::vector<Track> sessions;
   for(const auto& track:*tracks)if(sessions.empty()||sessions.back().session!=track.session)sessions.push_back(track);
   auto session=sessions.back().session;
   if(sessions.size()>1){
    CdTrackDialog choose(owner,sessions,true,true);
    if(choose.DoModal()!=IDOK)return;
    session=sessions[choose.selected].session;
   }
   for(const auto& track:*tracks)if(track.session==session)selection.push_back(track);
  }else{
   auto candidates=format==RawOutput::Form2Track?DataTracks(*tracks):*tracks;
   if(!candidates.empty()){
    CdTrackDialog choose(owner,candidates,true);
    if(choose.DoModal()!=IDOK)return;
    selection.push_back(candidates[choose.selected]);
   }
  }
  if(selection.empty()){AfxMessageBox("The disc has no tracks for the selected format.");return;}
  const auto destination=options.output;
  BackgroundCopies::StartStorage(SaveRawCd(options.drive,std::move(options.output),{format,std::move(selection),*generation},source),
     owner,format==RawOutput::WaveTracks?"Extracting CD audio":"Saving raw CD image","Optical extraction",{},{true,nullptr,destination});
  return;
 }
 auto source=MakeNativeCdImageSource();
 auto inspected=ShowStorageWaitJob(MakeCdTrackInspectionJob(options.drive,source,tracks,generation),false,owner);
 if(inspected.code){if(inspected.error!=ERROR_CANCELLED)AfxMessageBox(inspected.message.c_str(),MB_OK|MB_ICONERROR);return;}
 CdTrackDialog choose(owner,*tracks);if(choose.DoModal()!=IDOK)return;
 CdImageSelection selection;selection.track=tracks->at(choose.selected).number;selection.expected=tracks->at(choose.selected);selection.identityKnown=true;selection.generation=*generation;selection.macVolumeOnly=choose.macOnly;
 BOOL lost=FALSE;
 const auto size=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,options.output.c_str(),-1,nullptr,0,nullptr,nullptr);
 std::string output(size,0);
 if(!size||!WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,options.output.c_str(),-1,output.data(),size,nullptr,&lost)||lost){
  AfxMessageBox("The cooked image output path cannot be represented on this system.");return;
 }
 output.pop_back();
 BackgroundCopies::StartStorage(MakeCdImageJob(options.drive,std::move(output),source,selection),
     owner,"Saving CD image","CD image dump",{},{true,nullptr,options.output});
}

#ifdef HFV_TESTING
bool TestCdTrackPicker(const std::vector<CdMedia::Track>& tracks){CWnd owner;auto klass=AfxRegisterWndClass(0);if(!owner.CreateEx(0,klass,"Hidden track-picker test",WS_OVERLAPPED,0,0,100,100,nullptr,0))return false;CdTrackDialog dialog(&owner,tracks);bool ok=dialog.Create(IDD_CD_DATA_TRACK,&owner)!=FALSE;
 if(ok){ok=dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCOUNT)==tracks.size()&&dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_GETCURSEL)==tracks.size()-1;dialog.SendDlgItemMessage(IDC_CDI_TRACK,CB_SETCURSEL,0);dialog.CheckDlgButton(IDC_CDI_MAC_ONLY,BST_CHECKED);ok=ok&&dialog.Capture()&&dialog.selected==0&&dialog.macOnly;dialog.DestroyWindow();}owner.DestroyWindow();return ok;}
#endif
