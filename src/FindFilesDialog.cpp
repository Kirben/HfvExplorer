// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "FindFilesDialog.h"
#include "BrowserPresentation.h"
#include "StorageJobDialog.h"
namespace {
class FindOptions : public CDialog {
public:
 FindRequest request;
 FindOptions(CWnd* owner,FindLocation location):CDialog(IDD_FIND_FILES,owner){request.location=std::move(location);}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();CheckDlgButton(IDC_FIND_RECURSIVE,BST_CHECKED);
  if(request.location.UsesUnicode()){BrowserPresentation::UnicodeEdit(m_hWnd,IDC_FIND_PATTERN);BrowserPresentation::UnicodeControl(m_hWnd,IDC_FIND_SCOPE,L"STATIC");SetDlgItemTextW(m_hWnd,IDC_FIND_SCOPE,BrowserPresentation::FromUtf8(request.location.directory).c_str());}else SetDlgItemText(IDC_FIND_SCOPE,request.location.directory.c_str());
  ((CEdit*)GetDlgItem(IDC_FIND_PATTERN))->LimitText(255);GetDlgItem(IDC_FIND_PATTERN)->SetFocus();return FALSE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)==IDC_FIND_WHOLE){auto text=IsDlgButtonChecked(IDC_FIND_WHOLE)?request.location.root:request.location.directory;if(request.location.UsesUnicode())SetDlgItemTextW(m_hWnd,IDC_FIND_SCOPE,BrowserPresentation::FromUtf8(text).c_str());else SetDlgItemText(IDC_FIND_SCOPE,text.c_str());return TRUE;}
  return CDialog::OnCommand(w,l);
 }
 void OnOK() override {
  auto widePattern=BrowserPresentation::Text(::GetDlgItem(m_hWnd,IDC_FIND_PATTERN));
  if(widePattern.empty()){AfxMessageBox("Enter a filename, part of a name, or a wildcard pattern.");return;}
  CString pattern;if(!request.location.UsesUnicode())GetDlgItemText(IDC_FIND_PATTERN,pattern);
  request.pattern=request.location.UsesUnicode()?BrowserPresentation::Utf8(std::u16string(widePattern.begin(),widePattern.end())):std::string((LPCSTR)pattern);request.recursive=IsDlgButtonChecked(IDC_FIND_RECURSIVE)!=0;
  request.caseSensitive=IsDlgButtonChecked(IDC_FIND_CASE)!=0;request.includeFolders=IsDlgButtonChecked(IDC_FIND_FOLDERS)!=0;
  if(IsDlgButtonChecked(IDC_FIND_WHOLE)){request.location.directory=request.location.root;if(!request.location.mac)request.location.origin.SetWindows(request.location.windowsRoot);if(request.location.native){request.location.nativeFolder=2;request.location.nativePath={2};}}
  CDialog::OnOK();
 }
};
class ResultsDialog : public CDialog {
 const FindResults& results_;bool incomplete_;bool mac_;
public:
 FindHit selected;
 ResultsDialog(CWnd* owner,const FindResults& results,bool incomplete,bool mac):CDialog(IDD_FIND_RESULTS,owner),results_(results),incomplete_(incomplete),mac_(mac){INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();auto list=(CListCtrl*)GetDlgItem(IDC_FIND_RESULTS);list->SetExtendedStyle(LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);
  CRect rect;list->GetClientRect(&rect);int width=rect.Width()-GetSystemMetrics(SM_CXVSCROLL)-4;
  if(results_.utf8)list->SendMessage(CCM_SETUNICODEFORMAT,TRUE);
  list->InsertColumn(0,"Name",LVCFMT_LEFT,width*23/100);list->InsertColumn(1,"Containing folder",LVCFMT_LEFT,width*49/100);
  list->InsertColumn(2,mac_?"Bytes (both forks)":"Bytes",LVCFMT_RIGHT,width*18/100);list->InsertColumn(3,"Kind",LVCFMT_LEFT,width*10/100);
  list->SetItemCountEx(int(results_.hits.size()),LVSICF_NOINVALIDATEALL|LVSICF_NOSCROLL);
  CString summary;summary.Format("%u matches; %llu entries checked",unsigned(results_.hits.size()),results_.checked);
  if(results_.truncated)summary+=". Result limit reached; refine the search.";
  if(results_.unreadable){CString warning;warning.Format(". %u folders could not be read.",results_.unreadable);summary+=warning;}
  if(incomplete_)summary+=". Search stopped with an error; results are incomplete.";
  SetDlgItemText(IDC_FIND_SUMMARY,summary);
  GetDlgItem(IDOK)->EnableWindow(!results_.hits.empty());if(!results_.hits.empty())list->SetItemState(0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
  return TRUE;
 }
 BOOL OnNotify(WPARAM w,LPARAM l,LRESULT* value) override {
  auto header=reinterpret_cast<NMHDR*>(l);
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==LVN_GETDISPINFOW){
   auto info=reinterpret_cast<NMLVDISPINFOW*>(l);int row=info->item.iItem;
   if((info->item.mask&LVIF_TEXT)&&row>=0&&size_t(row)<results_.hits.size()){
    const auto& hit=results_.hits[row];std::wstring text;
    switch(info->item.iSubItem){case 0:text=BrowserPresentation::FromUtf8(hit.name);break;case 1:text=BrowserPresentation::FromUtf8(hit.parent);break;
     case 2:if(!hit.folder)text=std::to_wstring(hit.bytes);break;case 3:text=hit.folder?L"Folder":L"File";break;}
    if(info->item.pszText&&info->item.cchTextMax>0)wcsncpy_s(info->item.pszText,info->item.cchTextMax,text.c_str(),_TRUNCATE);
   }*value=0;return TRUE;
  }
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==LVN_GETDISPINFOA){
   auto info=reinterpret_cast<NMLVDISPINFOA*>(l);int row=info->item.iItem;
   if((info->item.mask&LVIF_TEXT)&&row>=0&&size_t(row)<results_.hits.size()){
    const auto& hit=results_.hits[row];std::string text;
    switch(info->item.iSubItem){case 0:text=hit.name;break;case 1:text=hit.parent;break;case 2:if(!hit.folder)text=std::to_string(hit.bytes);break;case 3:text=hit.folder?"Folder":"File";break;}
    if(info->item.pszText&&info->item.cchTextMax>0)strncpy_s(info->item.pszText,info->item.cchTextMax,text.c_str(),_TRUNCATE);
   }*value=0;return TRUE;
  }
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==NM_DBLCLK){OnOK();*value=0;return TRUE;}
  return CDialog::OnNotify(w,l,value);
 }
 bool PickResult() {
  int row=((CListCtrl*)GetDlgItem(IDC_FIND_RESULTS))->GetNextItem(-1,LVNI_SELECTED);
  if(row<0||size_t(row)>=results_.hits.size())return false;selected=results_.hits[row];return true;
 }
 void OnOK() override {if(PickResult())CDialog::OnOK();}
};
}
#ifdef HFV_TESTING
bool ProbeFindResultsDialog(CWnd* owner,const FindResults& results,bool mac){
 ResultsDialog dialog(owner,results,false,mac);if(!dialog.Create(IDD_FIND_RESULTS,owner))return false;
 bool ok=!dialog.IsWindowVisible();
 if(results.hits.empty())ok=ok&&!dialog.GetDlgItem(IDOK)->IsWindowEnabled();
 else{
  char text[MAX_PATH]={};NMLVDISPINFOA info={};info.hdr.hwndFrom=dialog.GetDlgItem(IDC_FIND_RESULTS)->GetSafeHwnd();info.hdr.idFrom=IDC_FIND_RESULTS;info.hdr.code=LVN_GETDISPINFOA;
  info.item.mask=LVIF_TEXT;info.item.iItem=0;info.item.iSubItem=0;info.item.pszText=text;info.item.cchTextMax=MAX_PATH;
  dialog.SendMessage(WM_NOTIFY,IDC_FIND_RESULTS,reinterpret_cast<LPARAM>(&info));ok=ok&&results.hits[0].name==text;
  info.item.iSubItem=2;dialog.SendMessage(WM_NOTIFY,IDC_FIND_RESULTS,reinterpret_cast<LPARAM>(&info));
  ok=ok&&(results.hits[0].folder?std::string():std::to_string(results.hits[0].bytes))==text;
  ok=ok&&dialog.PickResult()&&dialog.selected.path==results.hits[0].path;
 }
 dialog.DestroyWindow();return ok;
}
#endif
bool ShowFindFilesDialog(CWnd* owner,FindLocation location,FindHit& selected){
 FindOptions options(owner,std::move(location));if(options.DoModal()!=IDOK)return false;
 auto results=std::make_shared<FindResults>();auto result=ShowStorageJob(MakeFindFilesJob(options.request,results),"Finding files",false,true,true);
 if(result.error==ERROR_CANCELLED)return false;
 if(result.code)AfxMessageBox(result.message.c_str(),MB_OK|MB_ICONERROR);
 if(result.code&&results->hits.empty())return false;
 ResultsDialog dialog(owner,*results,result.code!=0,options.request.location.mac);if(dialog.DoModal()!=IDOK)return false;
 selected=std::move(dialog.selected);return true;
}
