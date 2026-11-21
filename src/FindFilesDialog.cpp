// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DialogDpi.h"
#include "resource.h"
#include "FindFilesDialog.h"
#include "BrowserPresentation.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
namespace {
class FindOptions : public CDialog {
public:
 FindRequest request;
 FindOptions(CWnd* owner,FindScope scope):CDialog(IDD_FIND_FILES,owner){request.scope=std::move(scope);}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();CheckDlgButton(IDC_FIND_RECURSIVE,BST_CHECKED);
  if(request.UsesUnicode())BrowserPresentation::UnicodeEdit(m_hWnd,IDC_FIND_PATTERN);
  BrowserPresentation::UnicodeControl(m_hWnd,IDC_FIND_SCOPE,L"STATIC");
  SetDlgItemTextW(m_hWnd,IDC_FIND_SCOPE,request.scope.folderLabel.c_str());
  GetDlgItem(IDC_FIND_WHOLE)->EnableWindow(!request.scope.volumeRoot.IsNone());
  ((CEdit*)GetDlgItem(IDC_FIND_PATTERN))->LimitText(255);GetDlgItem(IDC_FIND_PATTERN)->SetFocus();return FALSE;
 }
 BOOL OnCommand(WPARAM w,LPARAM l) override {
  if(LOWORD(w)==IDC_FIND_WHOLE){
   const auto& text=IsDlgButtonChecked(IDC_FIND_WHOLE)?request.scope.volumeLabel:request.scope.folderLabel;
   SetDlgItemTextW(m_hWnd,IDC_FIND_SCOPE,text.c_str());
   return TRUE;
  }
  return CDialog::OnCommand(w,l);
 }
 void OnOK() override {
  auto widePattern=BrowserPresentation::Text(::GetDlgItem(m_hWnd,IDC_FIND_PATTERN));
  if(widePattern.empty()){AfxMessageBox("Enter a filename, part of a name, or a wildcard pattern.");return;}
  CString pattern;if(!request.UsesUnicode())GetDlgItemText(IDC_FIND_PATTERN,pattern);
  request.pattern=request.UsesUnicode()?BrowserPresentation::Utf8(std::u16string(widePattern.begin(),widePattern.end())):std::string((LPCSTR)pattern);request.recursive=IsDlgButtonChecked(IDC_FIND_RECURSIVE)!=0;
  request.caseSensitive=IsDlgButtonChecked(IDC_FIND_CASE)!=0;request.includeFolders=IsDlgButtonChecked(IDC_FIND_FOLDERS)!=0;
  request.wholeVolume=IsDlgButtonChecked(IDC_FIND_WHOLE)==BST_CHECKED;
  CDialog::OnOK();
 }
};
class ResultsDialog : public CDialog {
 std::shared_ptr<const FindResults> results_;
 bool incomplete_,mac_,selfOwned_=false;
 std::function<void(FindHit)> accepted_;
 UINT dpi_ = NativeDpi::Base;
 NativeDpi::Measurement columns_[4];
 LRESULT WindowProc(UINT message,WPARAM wParam,LPARAM lParam) override {
  if(selfOwned_&&message==WM_CLOSE){DestroyWindow();return 0;}
  const UINT next=LOWORD(wParam);
  if(message==NativeDpi::Changed&&next&&next!=dpi_){
   auto* list=static_cast<CListCtrl*>(GetDlgItem(IDC_FIND_RESULTS));
   if(list){
    int widths[ARRAYSIZE(columns_)]{};
    for(int i=0;i<ARRAYSIZE(columns_);++i)
     widths[i]=columns_[i].Resize(list->GetColumnWidth(i),dpi_,next);
    dpi_=next;
    const auto result=CDialog::WindowProc(message,wParam,lParam);
    for(int i=0;i<ARRAYSIZE(columns_);++i)list->SetColumnWidth(i,widths[i]);
    return result;
   }
  }
  return CDialog::WindowProc(message,wParam,lParam);
 }
public:
 FindHit selected;
 ResultsDialog(CWnd* owner,std::shared_ptr<const FindResults> results,bool incomplete,bool mac):CDialog(IDD_FIND_RESULTS,owner),results_(std::move(results)),incomplete_(incomplete),mac_(mac){INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);}
 BOOL OnInitDialog() override {
  CDialog::OnInitDialog();auto list=(CListCtrl*)GetDlgItem(IDC_FIND_RESULTS);list->SetExtendedStyle(LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);
  dpi_=NativeDpi::Window(m_hWnd);
  CRect rect;list->GetClientRect(&rect);
  int width=rect.Width()-NativeDpi::Metric(SM_CXVSCROLL,dpi_)-NativeDpi::Scale(4,dpi_);
  if(results_->utf8)list->SendMessage(CCM_SETUNICODEFORMAT,TRUE);
  list->InsertColumn(0,"Name",LVCFMT_LEFT,width*23/100);list->InsertColumn(1,"Containing folder",LVCFMT_LEFT,width*49/100);
  list->InsertColumn(2,mac_?"Bytes (both forks)":"Bytes",LVCFMT_RIGHT,width*18/100);list->InsertColumn(3,"Kind",LVCFMT_LEFT,width*10/100);
  list->SetItemCountEx(int(results_->hits.size()),LVSICF_NOINVALIDATEALL|LVSICF_NOSCROLL);
  CString summary;summary.Format("%u matches; %llu entries checked",unsigned(results_->hits.size()),results_->checked);
  if(results_->truncated)summary+=". Result limit reached; refine the search.";
  if(results_->unreadable){CString warning;warning.Format(". %u folders could not be read.",results_->unreadable);summary+=warning;}
  if(incomplete_)summary+=". Search stopped with an error; results are incomplete.";
  SetDlgItemText(IDC_FIND_SUMMARY,summary);
  GetDlgItem(IDOK)->EnableWindow(!results_->hits.empty());if(!results_->hits.empty())list->SetItemState(0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
  return TRUE;
 }
 BOOL OnNotify(WPARAM w,LPARAM l,LRESULT* value) override {
  auto header=reinterpret_cast<NMHDR*>(l);
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==LVN_GETDISPINFOW){
   auto info=reinterpret_cast<NMLVDISPINFOW*>(l);int row=info->item.iItem;
   if((info->item.mask&LVIF_TEXT)&&row>=0&&size_t(row)<results_->hits.size()){
    const auto& hit=results_->hits[row];std::wstring text;
    switch(info->item.iSubItem){case 0:text=BrowserPresentation::FromUtf8(hit.name);break;case 1:text=BrowserPresentation::FromUtf8(hit.parent);break;
     case 2:if(!hit.folder)text=std::to_wstring(hit.bytes);break;case 3:text=hit.folder?L"Folder":L"File";break;}
    if(info->item.pszText&&info->item.cchTextMax>0)wcsncpy_s(info->item.pszText,info->item.cchTextMax,text.c_str(),_TRUNCATE);
   }*value=0;return TRUE;
  }
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==LVN_GETDISPINFOA){
   auto info=reinterpret_cast<NMLVDISPINFOA*>(l);int row=info->item.iItem;
   if((info->item.mask&LVIF_TEXT)&&row>=0&&size_t(row)<results_->hits.size()){
    const auto& hit=results_->hits[row];std::string text;
    switch(info->item.iSubItem){case 0:text=hit.name;break;case 1:text=hit.parent;break;case 2:if(!hit.folder)text=std::to_string(hit.bytes);break;case 3:text=hit.folder?"Folder":"File";break;}
    if(info->item.pszText&&info->item.cchTextMax>0)strncpy_s(info->item.pszText,info->item.cchTextMax,text.c_str(),_TRUNCATE);
   }*value=0;return TRUE;
  }
  if(header->idFrom==IDC_FIND_RESULTS&&header->code==NM_DBLCLK){OnOK();*value=0;return TRUE;}
  return CDialog::OnNotify(w,l,value);
 }
 bool PickResult() {
  int row=((CListCtrl*)GetDlgItem(IDC_FIND_RESULTS))->GetNextItem(-1,LVNI_SELECTED);
  if(row<0||size_t(row)>=results_->hits.size())return false;selected=results_->hits[row];return true;
 }
 void OnOK() override {
  if(!PickResult())return;
  if(!selfOwned_){CDialog::OnOK();return;}
  auto accepted=std::move(accepted_);
  auto hit=std::move(selected);
  DestroyWindow();
  if(accepted)accepted(std::move(hit));
 }
 void OnCancel() override {if(selfOwned_)DestroyWindow();else CDialog::OnCancel();}
 void PostNcDestroy() override {if(selfOwned_)delete this;}

 static HWND Open(CWnd& owner,std::shared_ptr<const FindResults> results,bool incomplete,bool mac,
                  std::function<void(FindHit)> accepted){
  auto dialog=std::make_unique<ResultsDialog>(&owner,std::move(results),incomplete,mac);
  dialog->accepted_=std::move(accepted);
  if(!dialog->Create(IDD_FIND_RESULTS,&owner))throw std::runtime_error("Cannot create search results");
  dialog->selfOwned_=true;
  auto* window=dialog.release();
  const auto hwnd=window->GetSafeHwnd();
#ifdef HFV_TESTING
  if(!owner.IsWindowVisible())return hwnd;
#endif
  window->ShowWindow(SW_SHOWNOACTIVATE);
  return hwnd;
 }

};
}
#ifdef HFV_TESTING
bool ProbeFindOptionsDialog(CWnd* owner){
 FindScope scope;
 scope.volumeRoot=FileOperations::Location::Windows(L"C:\\");
 scope.folderLabel=L"Macintosh / Documents";
 scope.volumeLabel=L"Macintosh";
 FindOptions dialog(owner,scope);
 if(!dialog.Create(IDD_FIND_FILES,owner))return false;
 const auto text=[&]{return BrowserPresentation::Text(::GetDlgItem(dialog.m_hWnd,IDC_FIND_SCOPE));};
 bool ok=text()==scope.folderLabel&&dialog.IsDlgButtonChecked(IDC_FIND_RECURSIVE)==BST_CHECKED&&
     ::IsWindowUnicode(::GetDlgItem(dialog.m_hWnd,IDC_FIND_SCOPE));
 dialog.CheckDlgButton(IDC_FIND_WHOLE,BST_CHECKED);
 dialog.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_FIND_WHOLE,BN_CLICKED));
 ok=ok&&text()==scope.volumeLabel;
 dialog.CheckDlgButton(IDC_FIND_WHOLE,BST_UNCHECKED);
 dialog.SendMessage(WM_COMMAND,MAKEWPARAM(IDC_FIND_WHOLE,BN_CLICKED));
 ok=ok&&text()==scope.folderLabel;
 dialog.DestroyWindow();
 return ok;
}
bool ProbeFindResultsDialog(CWnd* owner,const FindResults& results,bool mac){
 ResultsDialog dialog(owner,std::make_shared<FindResults>(results),false,mac);if(!dialog.Create(IDD_FIND_RESULTS,owner))return false;
 bool ok=!dialog.IsWindowVisible();
 auto& list=*static_cast<CListCtrl*>(dialog.GetDlgItem(IDC_FIND_RESULTS));
 const UINT originalDpi=NativeDpi::Window(dialog.GetSafeHwnd());
 const int column=list.GetColumnWidth(0);
 RECT bounds{};
 dialog.GetWindowRect(&bounds);
 const UINT nextDpi=originalDpi*2;
 dialog.SendMessage(NativeDpi::Changed,MAKEWPARAM(nextDpi,nextDpi),LPARAM(&bounds));
 ok=ok&&list.GetColumnWidth(0)==2*column;
 dialog.SendMessage(NativeDpi::Changed,MAKEWPARAM(originalDpi,originalDpi),LPARAM(&bounds));
 ok=ok&&list.GetColumnWidth(0)==column;
 if(results.hits.empty())ok=ok&&!dialog.GetDlgItem(IDOK)->IsWindowEnabled();
 else{
  char text[MAX_PATH]={};NMLVDISPINFOA info={};info.hdr.hwndFrom=dialog.GetDlgItem(IDC_FIND_RESULTS)->GetSafeHwnd();info.hdr.idFrom=IDC_FIND_RESULTS;info.hdr.code=LVN_GETDISPINFOA;
  info.item.mask=LVIF_TEXT;info.item.iItem=0;info.item.iSubItem=0;info.item.pszText=text;info.item.cchTextMax=MAX_PATH;
  dialog.SendMessage(WM_NOTIFY,IDC_FIND_RESULTS,reinterpret_cast<LPARAM>(&info));ok=ok&&results.hits[0].name==text;
  info.item.iSubItem=2;dialog.SendMessage(WM_NOTIFY,IDC_FIND_RESULTS,reinterpret_cast<LPARAM>(&info));
  ok=ok&&(results.hits[0].folder?std::string():std::to_string(results.hits[0].bytes))==text;
  ok=ok&&dialog.PickResult()&&dialog.selected.path==results.hits[0].path;
 }
 dialog.DestroyWindow();
 if(!results.hits.empty()){
  bool picked=false;
  auto shared=std::make_shared<FindResults>(results);
  const auto modeless=ResultsDialog::Open(*owner,shared,false,mac,[&](FindHit hit){
   picked=hit.path==results.hits.front().path;
  });
  if(!owner->IsWindowEnabled()||!IsWindow(modeless))
   throw std::runtime_error("Modeless Find results did not preserve the browser owner");
  SendMessage(modeless,WM_COMMAND,IDOK,0);
  if(!picked||IsWindow(modeless)){
   if(IsWindow(modeless))::DestroyWindow(modeless);
   throw std::runtime_error("Selecting a result did not deliver the hit and close the modeless window");
  }
  auto cancel=ResultsDialog::Open(*owner,shared,false,mac,[&](FindHit){ok=false;});
  SendMessage(cancel,WM_CLOSE,0,0);
  if(IsWindow(cancel)){
   ::DestroyWindow(cancel);
   throw std::runtime_error("Closing modeless Find results left the window open");
  }
 }
 return ok;
}
#endif
bool StartFindSearch(BrowserSession& session,FindRequest request,FindCompletion completed)
{
 auto results=std::make_shared<FindResults>();
 return BackgroundCopies::StartStorageResult(MakeFindFilesJob(std::move(request),results),session.Owner(),
     "Finding files","Search",session.WhileOpen([results,completed=std::move(completed)]
     (BrowserSession& origin,const Storage::Result& result){
  if(completed)completed(origin,results,result);
 }),{true});
}
void ShowFindFilesDialog(BrowserSession& session,FindScope scope)
{
 FindOptions options(session.Owner(),std::move(scope));
 if(options.DoModal()!=IDOK)return;
 const bool mac=!options.request.Origin().IsWindows();
 StartFindSearch(session,std::move(options.request),[mac](BrowserSession& origin,
                std::shared_ptr<const FindResults> results,const Storage::Result& result){
  if(result.error==ERROR_CANCELLED||(result.code&&results->hits.empty()))return;
  ResultsDialog::Open(*origin.Owner(),std::move(results),result.code!=0,mac,origin.WhileOpen(
   [](BrowserSession& browser,FindHit selected){
    if(!browser.RevealFindHit(selected))browser.RejectNavigation({},"The search volume is no longer attached.");
   }));
 });
}
