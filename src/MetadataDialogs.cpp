// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DialogDpi.h"
#include "HFVExplorer.h"
#include "AskProperties.h"
#include "MetadataDialogs.h"
#include "BackgroundCopies.h"
#include "BrowserPresentation.h"
#include "charset.h"
#include "HfsPlusDisk.h"
#include "HfsPlusDialogs.h"
#include <algorithm>
#include <string_view>
namespace MetadataDialogs {
namespace {
std::vector<std::unique_ptr<CDialog>> windows;
struct DialogActivity {virtual ~DialogActivity()=default;unsigned dispatchDepth=0;};
struct DialogDispatch {DialogActivity& dialog;explicit DialogDispatch(DialogActivity& d):dialog(d){++dialog.dispatchDepth;}~DialogDispatch(){--dialog.dispatchDepth;}};
void CollectClosed(){windows.erase(std::remove_if(windows.begin(),windows.end(),[](const std::unique_ptr<CDialog>& window){auto active=dynamic_cast<DialogActivity*>(window.get());return !IsWindow(window->GetSafeHwnd())&&(!active||!active->dispatchDepth);}),windows.end());}
#ifdef HFV_TESTING
std::string lastError;unsigned nestedRetained=0;
#endif
bool NestedClose(UINT message){
#ifdef HFV_TESTING
 if(message==WM_APP+173){Shutdown();CollectClosed();nestedRetained=unsigned(windows.size());return true;}
#else
 (void)message;
#endif
 return false;
}
#define METADATA_DISPATCH(Base) LRESULT WindowProc(UINT message,WPARAM w,LPARAM l)override{DialogDispatch dispatch(*this);if(NestedClose(message))return 0;return Base::WindowProc(message,w,l);}
void Error(HWND window,const std::exception& error,bool hidden){
#ifdef HFV_TESTING
 lastError=error.what();if(hidden)return;
#else
 (void)hidden;
#endif
 ::MessageBoxA(window,error.what(),"Extended attributes",MB_OK|MB_ICONERROR);
}

struct AttributeState {FileOperations::Location source;HfsPlus::Attributes original,desired;HWND window=nullptr;bool writable=false,importing=false;};
std::wstring Text(HWND window,int id){auto control=GetDlgItem(window,id);int n=GetWindowTextLengthW(control);std::wstring text(n+1,0);GetWindowTextW(control,&text[0],n+1);text.resize(n);return text;}
struct AttributesDialog:CDialog,DialogActivity {
 METADATA_DISPATCH(CDialog)
 std::shared_ptr<AttributeState> state=std::make_shared<AttributeState>();bool hidden,hexEditable=true;uint64_t pageOffset=0,pageLength=0;std::u16string pageName;
 AttributesDialog(FileOperations::Location source,const FileOperations::Completion& value,bool h):CDialog(IDD_EXTENDED_ATTRIBUTES,CWnd::GetDesktopWindow()),hidden(h){state->source=std::move(source);state->desired=state->original=value.attributes;state->writable=value.capabilities.CanEditAttributes()&&!value.entry.protectedFile&&value.entry.id!=2;}
 std::u16string Name(){return MacBrowser::U16(Text(m_hWnd,IDC_ATTR_NAME));}
 void Refresh(){SendDlgItemMessage(IDC_ATTR_LIST,LB_RESETCONTENT);for(const auto& a:state->desired){auto text=MacBrowser::Wide(a.first)+L"  ("+std::to_wstring(a.second.size())+L" bytes)";SendDlgItemMessageW(m_hWnd,IDC_ATTR_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}}
 void Clear(){SetDlgItemText(IDC_ATTR_OFFSET,"0");GetDlgItem(IDC_ATTR_GO)->EnableWindow(FALSE);pageOffset=pageLength=0;pageName.clear();GetDlgItem(IDC_ATTR_PREVIOUS)->EnableWindow(FALSE);GetDlgItem(IDC_ATTR_NEXT)->EnableWindow(FALSE);SetDlgItemTextW(m_hWnd,IDC_ATTR_NAME,L"");SetDlgItemTextW(m_hWnd,IDC_ATTR_VALUE,L"");SetDlgItemTextW(m_hWnd,IDC_ATTR_DETAIL,L"Set value stages a change. Apply saves all staged changes.");hexEditable=true;SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETREADONLY,!state->writable);}
 void Page(){GetDlgItem(IDC_ATTR_GO)->EnableWindow(TRUE);SetDlgItemTextW(m_hWnd,IDC_ATTR_OFFSET,std::to_wstring(pageOffset).c_str());auto it=state->desired.find(pageName);if(it==state->desired.end())return;const auto& value=it->second;pageLength=(std::min)(uint64_t(65536),value.size()-pageOffset);HfsPlus::Bytes page(size_t(pageLength),0);value.Read(pageOffset,page.data(),page.size());std::wstring hex;const wchar_t* digits=L"0123456789ABCDEF";for(size_t i=0;i<page.size();++i){auto b=page[i];hex+=digits[b>>4];hex+=digits[b&15];hex+=(i%16==15?L"\r\n":L" ");}
  SetDlgItemTextW(m_hWnd,IDC_ATTR_VALUE,hex.c_str());SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETREADONLY,!state->writable);GetDlgItem(IDC_ATTR_PREVIOUS)->EnableWindow(pageOffset!=0);GetDlgItem(IDC_ATTR_NEXT)->EnableWindow(pageOffset+pageLength<value.size());
  auto detail=L"Bytes "+std::to_wstring(pageOffset)+L" - "+std::to_wstring(pageOffset+pageLength)+L" of "+std::to_wstring(value.size())+L". Set value stages this page; Apply saves staged changes.";SetDlgItemTextW(m_hWnd,IDC_ATTR_DETAIL,detail.c_str());
 }
 void Selected(){int selected=int(SendDlgItemMessage(IDC_ATTR_LIST,LB_GETCURSEL));if(selected<0||size_t(selected)>=state->desired.size())return;auto it=state->desired.begin();std::advance(it,selected);pageName=it->first;pageOffset=0;SetDlgItemTextW(m_hWnd,IDC_ATTR_NAME,MacBrowser::Wide(pageName).c_str());Page();}
 void StageValue(){auto text=Text(m_hWnd,IDC_ATTR_VALUE);HfsPlus::Bytes bytes;int half=-1;for(auto c:text){if(iswspace(c))continue;int n=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:-1;if(n<0)throw HfsPlus::Error("Enter hexadecimal byte pairs (00 through FF)");if(half<0)half=n;else{bytes.push_back(uint8_t(half*16+n));half=-1;}}if(half>=0)throw HfsPlus::Error("A hexadecimal byte needs two digits");auto next=state->desired;auto name=Name();if(!pageName.empty()&&state->desired.count(pageName)&&state->desired.at(pageName).size()>65536&&name!=pageName)throw HfsPlus::Error("Keep the selected large attribute name unchanged; use New or Import value to create another attribute");if(name==pageName&&next.count(name)&&next.at(name).size()>65536){if(bytes.size()!=pageLength)throw HfsPlus::Error("Keep this page length unchanged; Import value can replace the entire attribute");next[name]=next.at(name).Patch(pageOffset,bytes);}else next[name]=std::move(bytes);HfsPlus::ValidateAttributes(next);state->desired=std::move(next);Refresh();}
 std::vector<std::pair<int,BOOL>> importControls;
 void FinishImport(bool success){state->importing=false;for(auto control:importControls)if(auto window=GetDlgItem(control.first))window->EnableWindow(control.second);importControls.clear();if(!success)SetDlgItemText(IDC_ATTR_DETAIL,"Import did not complete; current values are unchanged.");}
 bool BeginImport(FileOperations::Location file,const std::u16string& name){
  if(!state->writable||state->importing||!IsWindow(m_hWnd))return false;if(name.empty())throw HfsPlus::Error("Enter the attribute name first");
  FileOperations::ImportAttributeValue request{std::move(file), name};auto model=state;state->importing=true;
  for(int id:{IDOK,IDC_ATTR_IMPORT,IDC_ATTR_SET,IDC_ATTR_REMOVE,IDC_ATTR_NEW,IDC_ATTR_EXPORT,IDC_ATTR_LIST,IDC_ATTR_NAME,IDC_ATTR_VALUE,IDC_ATTR_OFFSET,IDC_ATTR_GO,IDC_ATTR_PREVIOUS,IDC_ATTR_NEXT})if(auto control=GetDlgItem(id)){importControls.push_back({id,control->IsWindowEnabled()});control->EnableWindow(FALSE);}
  SetDlgItemText(IDC_ATTR_DETAIL,"Reading attribute value; wait for completion before applying.");
  try{bool started=BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true,[model](bool success){model->importing=false;if(!model->window)return;auto dialog=dynamic_cast<AttributesDialog*>(CWnd::FromHandlePermanent(model->window));if(dialog&&dialog->state==model){DialogDispatch active(*dialog);dialog->FinishImport(success);}},[model](const FileOperations::Completion& value){if(!model->window)return;auto dialog=dynamic_cast<AttributesDialog*>(CWnd::FromHandlePermanent(model->window));if(!dialog||dialog->state!=model)return;DialogDispatch active(*dialog);try{auto next=model->desired;for(const auto& a:value.attributes)next[a.first]=a.second;HfsPlus::ValidateAttributes(next);model->desired=std::move(next);dialog->Refresh();dialog->Clear();}catch(const std::exception& error){Error(model->window,error,dialog->hidden);}});if(!started)FinishImport(false);return started;}catch(...){FinishImport(false);throw;}
 }
 BOOL OnInitDialog()override{CDialog::OnInitDialog();BrowserPresentation::UnicodeEdit(m_hWnd,IDC_ATTR_NAME);BrowserPresentation::UnicodeControl(m_hWnd,IDC_ATTR_LIST,L"LISTBOX");BrowserPresentation::UnicodeControl(m_hWnd,IDC_ATTR_DETAIL,L"STATIC");SendDlgItemMessage(IDC_ATTR_NAME,EM_SETLIMITTEXT,127);state->window=m_hWnd;SetDlgItemTextW(m_hWnd,IDC_CP_PHASE,state->writable?L"Named attributes and com.apple.system.Security (Mac ACL)":L"Read-only attributes and Mac ACL");SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETLIMITTEXT,262144);for(int id:{IDC_ATTR_SET,IDC_ATTR_REMOVE,IDC_ATTR_IMPORT,IDC_ATTR_NEW,IDOK})GetDlgItem(id)->EnableWindow(state->writable);Refresh();Clear();return TRUE;}
 void PostNcDestroy()override{state->window=nullptr;state->writable=false;state->source={};state->original.clear();state->desired.clear();CDialog::PostNcDestroy();}
 BOOL OnCommand(WPARAM wp,LPARAM lp)override{
  int id=LOWORD(wp);if(state->importing&&id!=IDCANCEL)return TRUE;try{
   if(id==IDC_ATTR_REFRESH){Refresh();Clear();return TRUE;}
   if(id==IDC_ATTR_LIST&&HIWORD(wp)==LBN_SELCHANGE){auto index=SendDlgItemMessage(IDC_ATTR_LIST,LB_GETCURSEL);if(index<0||size_t(index)>=state->desired.size())return TRUE;auto item=state->desired.begin();std::advance(item,index);auto selected=item->first;if(state->writable&&!pageName.empty())StageValue();auto found=state->desired.find(selected);if(found!=state->desired.end()){auto next=std::distance(state->desired.begin(),found);SendDlgItemMessage(IDC_ATTR_LIST,LB_SETCURSEL,next);Selected();}return TRUE;}
   if(id==IDC_ATTR_NEW){Clear();return TRUE;}
   if(id==IDC_ATTR_GO){auto text=Text(m_hWnd,IDC_ATTR_OFFSET);uint64_t offset=0;if(text.empty())throw HfsPlus::Error("Enter a decimal byte offset");for(auto c:text){if(c<L'0'||c>L'9'||offset>(UINT64_MAX-(c-L'0'))/10)throw HfsPlus::Error("Invalid byte offset");offset=offset*10+(c-L'0');}if(!state->desired.count(pageName)||(offset&&offset>=state->desired.at(pageName).size()))throw HfsPlus::Error("Byte offset lies outside the attribute");if(state->writable)StageValue();pageOffset=offset/65536*65536;Page();return TRUE;}
   if(id==IDC_ATTR_PREVIOUS||id==IDC_ATTR_NEXT){if(state->writable)StageValue();if(id==IDC_ATTR_PREVIOUS&&pageOffset)pageOffset-=65536;else if(id==IDC_ATTR_NEXT&&state->desired.count(pageName)&&pageOffset+pageLength<state->desired.at(pageName).size())pageOffset+=pageLength;Page();return TRUE;}
   if(id==IDC_ATTR_SET&&state->writable){StageValue();return TRUE;}
   if(id==IDC_ATTR_REMOVE&&state->writable){state->desired.erase(Name());Refresh();Clear();return TRUE;}
   if(id==IDC_ATTR_IMPORT&&state->writable){auto name=Name();if(name.empty())throw HfsPlus::Error("Enter the attribute name first");CFileDialog picker(TRUE,nullptr,nullptr,OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR,"Binary files (*.*)|*.*||",this);if(picker.DoModal()!=IDOK||!IsWindow(m_hWnd))return TRUE;FileOperations::Location file;file.SetWindows(VolumeCoord::Wide((LPCSTR)picker.GetPathName()));BeginImport(std::move(file),name);return TRUE;}
   if(id==IDC_ATTR_EXPORT){auto name=Name();if(!state->desired.count(name))throw HfsPlus::Error("Select an attribute first");CFileDialog picker(FALSE,"bin",nullptr,OFN_HIDEREADONLY|OFN_NOCHANGEDIR,"Binary files (*.bin)|*.bin|All files (*.*)|*.*||",this);if(picker.DoModal()!=IDOK||!IsWindow(m_hWnd))return TRUE;auto file=FileOperations::Location::Windows(VolumeCoord::Wide((LPCSTR)picker.GetPathName()));FileOperations::ExportAttributeValue request{std::move(file),state->desired.at(name)};BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true);return TRUE;}
  }catch(const std::exception& error){auto found=state->desired.find(pageName);SendDlgItemMessage(IDC_ATTR_LIST,LB_SETCURSEL,found==state->desired.end()?-1:std::distance(state->desired.begin(),found));Error(m_hWnd,error,hidden);return TRUE;}
  return CDialog::OnCommand(wp,lp);
 }
 void OnOK()override{if(!state->writable||state->importing)return;FileOperations::EditAttributes request{state->source,state->original,state->desired};if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();}
 void OnCancel()override{DestroyWindow();}
};
void PresentAttributes(FileOperations::Location source,const FileOperations::Completion& value,bool hidden,HWND anchor){CollectClosed();auto dialog=std::make_unique<AttributesDialog>(std::move(source),value,hidden);if(!dialog->Create(IDD_EXTENDED_ATTRIBUTES,CWnd::GetDesktopWindow()))throw HfsPlus::Error("Cannot create attribute editor");NativeDpi::CenterPopup(dialog->GetSafeHwnd(),anchor);if(!hidden)dialog->ShowWindow(SW_SHOWNOACTIVATE);windows.push_back(std::move(dialog));}
void PresentFinderInfo(FileOperations::Location,const MacBrowser::Entry&,bool,HWND);
bool IsApfs(const FileOperations::Location& source)
{
 const auto volume=source.Volume();
 return volume&&std::string_view(volume->Kind())=="APFS";
}
struct Summary:CDialog,DialogActivity {
 METADATA_DISPATCH(CDialog)
 FileOperations::Completion value;FileOperations::Location source;bool hidden;
 Summary(FileOperations::Location s,const FileOperations::Completion& v,bool h):CDialog(IDD_METADATA_SUMMARY,CWnd::GetDesktopWindow()),value(v),source(std::move(s)),hidden(h){}
 bool EditFinderInfo()const{return value.capabilities.CanEditBasicMetadata()&&value.entry.id!=2&&IsApfs(source);}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();for(int id:{IDC_CP_PHASE,IDC_CP_FILE,IDC_CP_STATUS})BrowserPresentation::UnicodeControl(m_hWnd,id,L"STATIC");auto e=value.entry;SetDlgItemTextW(m_hWnd,IDC_CP_PHASE,MacBrowser::Wide(e.name).c_str());
 auto description=BrowserPresentation::FromUtf8(value.description)+(value.capabilities.writable?L"":L" (read-only)");SetDlgItemTextW(m_hWnd,IDC_CP_FILE,description.c_str());
 auto sizes=e.directory?L"Folder":L"Data fork: "+std::to_wstring(e.forks[0].size)+L" bytes\r\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";if(e.hardLink)sizes+=L"\r\nHard links: "+std::to_wstring(e.linkCount);if(!value.details.empty())sizes+=L"\r\n"+BrowserPresentation::FromUtf8(value.details);SetDlgItemTextW(m_hWnd,IDC_CP_STATUS,sizes.c_str());GetDlgItem(IDC_ATTR_OPEN)->ShowWindow((value.capabilities.attributes||EditFinderInfo())?SW_SHOW:SW_HIDE);
 if(EditFinderInfo())SetDlgItemText(IDC_ATTR_OPEN,"Finder information...");
 bool compression=value.capabilities.compression&&!e.directory;GetDlgItem(IDC_FILE_COMPRESSION)->ShowWindow(compression?SW_SHOW:SW_HIDE);GetDlgItem(IDC_FILE_COMPRESSION)->EnableWindow(compression&&value.capabilities.CanEditCompression()&&!e.protectedFile&&e.readError.empty());SetDlgItemText(IDC_FILE_COMPRESSION,e.compressed?"Decompress":"Compress");
 for(int id:{IDC_COMPRESSION_LABEL,IDC_COMPRESSION_METHOD})GetDlgItem(id)->ShowWindow(compression?SW_SHOW:SW_HIDE);
 auto combo=static_cast<CComboBox*>(GetDlgItem(IDC_COMPRESSION_METHOD));
 for(const char* label:{"Uncompressed","zlib","LZVN","LZFSE","LZBITMAP","Stored (no size reduction)","Legacy stored (inline, up to 3786 bytes)"})combo->AddString(label);
 combo->SetCurSel(e.compressed?0:1);combo->EnableWindow(compression&&value.capabilities.CanEditCompression()&&!e.protectedFile&&e.readError.empty());return TRUE;}
 BOOL OnCommand(WPARAM wp,LPARAM lp)override{if(LOWORD(wp)==IDC_COMPRESSION_METHOD&&HIWORD(wp)==CBN_SELCHANGE){auto selected=SendDlgItemMessage(IDC_COMPRESSION_METHOD,CB_GETCURSEL);SetDlgItemText(IDC_FILE_COMPRESSION,selected?"Compress":"Decompress");return TRUE;}if(LOWORD(wp)==IDC_ATTR_OPEN){
  if(EditFinderInfo()){
   PresentFinderInfo(source,value.entry,hidden,m_hWnd);
   DestroyWindow();
  }else PresentAttributes(source,value,hidden,m_hWnd);
  return TRUE;
 }
 if(LOWORD(wp)==IDC_FILE_COMPRESSION){using Mode=HfsPlus::Volume::CompressionMode;const Mode methods[]={Mode::Uncompressed,Mode::Zlib,Mode::Lzvn,Mode::Lzfse,Mode::Lzbitmap,Mode::Stored,Mode::StoredInline};auto selected=SendDlgItemMessage(IDC_COMPRESSION_METHOD,CB_GETCURSEL);if(selected<0||selected>=ARRAYSIZE(methods))return TRUE;FileOperations::ChangeCompression request{source,value.entry,value.attributes,methods[selected]};if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();return TRUE;}return CDialog::OnCommand(wp,lp);}
 void PostNcDestroy()override{source={};value={};CDialog::PostNcDestroy();}
 void OnOK()override{DestroyWindow();}void OnCancel()override{DestroyWindow();}
};

HfsPlus::Bytes ParseFinderInfo(const std::wstring& text,size_t length)
{
 HfsPlus::Bytes bytes;
 int half=-1;
 for(auto c:text){
  if(iswspace(c))continue;
  const auto digit=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:-1;
  if(digit<0)throw HfsPlus::Error("Finder information requires hexadecimal bytes");
  if(half<0)half=digit;
  else{bytes.push_back(uint8_t(half*16+digit));half=-1;}
 }
 if(half>=0||bytes.size()!=length)
  throw HfsPlus::Error("Finder information must contain exactly "+std::to_string(length)+" bytes");
 return bytes;
}

struct ApfsEditor:CDialog,DialogActivity {
 METADATA_DISPATCH(CDialog)
 FileOperations::Location source;
 MacBrowser::Entry original;
 bool hidden;
 ApfsEditor(FileOperations::Location item,const MacBrowser::Entry& entry,bool testHidden)
     :CDialog(IDD_APFS_PROPERTIES,CWnd::GetDesktopWindow()),source(std::move(item)),original(entry),hidden(testHidden){}
 BOOL OnInitDialog()override{
  CDialog::OnInitDialog();
  HfvUnicodeNameEdit(m_hWnd,IDC_HPLUS_NAME);
  SetDlgItemTextW(m_hWnd,IDC_HPLUS_NAME,MacBrowser::Wide(original.name).c_str());
  std::wstring hex;
  for(const auto value:original.finder){
   wchar_t byte[4];
   swprintf_s(byte,L"%02X ",unsigned(value));
   hex+=byte;
  }
  SetDlgItemTextW(m_hWnd,IDC_MFS_FINDER,hex.c_str());
  return TRUE;
 }
 void OnOK()override{
  try{
   auto desired=original;
   const auto bytes=ParseFinderInfo(Text(m_hWnd,IDC_MFS_FINDER),desired.finder.size());
   std::copy(bytes.begin(),bytes.end(),desired.finder.begin());
   const FileOperations::EditBasicMetadata request{source,original,std::move(desired)};
   if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();
  }catch(const std::exception& error){
#ifdef HFV_TESTING
   if(hidden){lastError=error.what();return;}
#endif
   ::MessageBoxA(m_hWnd,error.what(),"APFS properties",MB_OK|MB_ICONERROR);
  }
 }
 void PostNcDestroy()override{source={};original={};CDialog::PostNcDestroy();}
 void OnCancel()override{DestroyWindow();}
};

void PresentFinderInfo(FileOperations::Location source,const MacBrowser::Entry& entry,bool hidden,HWND anchor)
{
 CollectClosed();
 auto editor=std::make_unique<ApfsEditor>(std::move(source),entry,hidden);
 if(!editor->Create(IDD_APFS_PROPERTIES,CWnd::GetDesktopWindow()))
  throw HfsPlus::Error("Cannot create Finder information editor");
 NativeDpi::CenterPopup(editor->GetSafeHwnd(),anchor);
 if(!hidden)editor->ShowWindow(SW_SHOWNOACTIVATE);
 windows.push_back(std::move(editor));
}

struct MfsEditor:CDialog,DialogActivity {
 METADATA_DISPATCH(CDialog)
 FileOperations::Location source;MacBrowser::Entry original;bool hidden,writable;
 MfsEditor(FileOperations::Location s,const FileOperations::Completion& value,bool h):CDialog(IDD_MFS_PROPERTIES,CWnd::GetDesktopWindow()),source(std::move(s)),original(value.entry),hidden(h),writable(value.capabilities.CanEditBasicMetadata()){}
 void SetDate(int id,uint32_t value){ULARGE_INTEGER ticks;ticks.QuadPart=(uint64_t(value)+9561628800ULL)*10000000ULL;FILETIME file{ticks.LowPart,ticks.HighPart};SYSTEMTIME time{};FileTimeToSystemTime(&file,&time);SendDlgItemMessageW(m_hWnd,id,DTM_SETFORMATW,0,reinterpret_cast<LPARAM>(L"yyyy-MM-dd HH:mm:ss"));SendDlgItemMessage(id,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&time));}
 uint32_t Date(int id){SYSTEMTIME time{};if(SendDlgItemMessage(id,DTM_GETSYSTEMTIME,0,reinterpret_cast<LPARAM>(&time))!=GDT_VALID)throw HfsPlus::Error("Choose a valid date");FILETIME file{};if(!SystemTimeToFileTime(&time,&file))throw HfsPlus::Error("Invalid date");ULARGE_INTEGER ticks;ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;auto seconds=ticks.QuadPart/10000000ULL;if(seconds<9561628800ULL||seconds-9561628800ULL>UINT32_MAX)throw HfsPlus::Error("MFS dates must be between 1904 and February 2040");return uint32_t(seconds-9561628800ULL);}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();HfvUnicodeNameEdit(m_hWnd,IDC_HPLUS_NAME);SetDlgItemTextW(m_hWnd,IDC_HPLUS_NAME,MacBrowser::Wide(original.name).c_str());SendDlgItemMessage(IDC_HPLUS_NAME,EM_SETLIMITTEXT,255);SetDlgItemInt(IDC_MFS_VERSION,original.version,FALSE);CheckDlgButton(IDC_MFS_LOCKED,original.protectedFile?BST_CHECKED:BST_UNCHECKED);SetDate(IDC_MFS_CREATED,original.created);SetDate(IDC_MFS_MODIFIED,original.modified);std::wstring hex;for(unsigned i=0;i<16;++i){wchar_t b[4];swprintf_s(b,L"%02X ",original.finder[i]);hex+=b;}SetDlgItemTextW(m_hWnd,IDC_MFS_FINDER,hex.c_str());for(int id:{IDC_HPLUS_NAME,IDC_MFS_VERSION,IDC_MFS_LOCKED,IDC_MFS_CREATED,IDC_MFS_MODIFIED,IDC_MFS_FINDER,IDOK})GetDlgItem(id)->EnableWindow(writable);return TRUE;}
 void OnOK()override{if(!writable)return;try{auto entry=original;std::string encoded,error;if(!MfsImage::EncodeName(Text(m_hWnd,IDC_HPLUS_NAME),encoded,error))throw HfsPlus::Error(error);entry.name=MacBrowser::U16(MfsDisplayName(encoded));auto text=Text(m_hWnd,IDC_MFS_VERSION);wchar_t* end=nullptr;auto version=wcstoul(text.c_str(),&end,10);if(text.empty()||*end||version>255)throw HfsPlus::Error("MFS file version must be 0-255");entry.version=version;entry.protectedFile=IsDlgButtonChecked(IDC_MFS_LOCKED)==BST_CHECKED;entry.created=Date(IDC_MFS_CREATED);entry.modified=Date(IDC_MFS_MODIFIED);entry.datesValid=true;
  const auto bytes=ParseFinderInfo(Text(m_hWnd,IDC_MFS_FINDER),16);std::copy(bytes.begin(),bytes.end(),entry.finder.begin());FileOperations::EditBasicMetadata request{source,original,std::move(entry)};if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();
 }catch(const std::exception& error){::MessageBoxA(m_hWnd,error.what(),"MFS properties",MB_OK|MB_ICONERROR);}}
 void PostNcDestroy()override{source={};original={};CDialog::PostNcDestroy();}
 void OnCancel()override{DestroyWindow();}
};
struct Editor:CAskProperties,DialogActivity {
 METADATA_DISPATCH(CAskProperties)
 FileOperations::Location source;hfsdirent original;bool hidden;
 Editor(FileOperations::Location s,const FileOperations::Completion& v,bool h):CAskProperties(CWnd::GetDesktopWindow()),source(std::move(s)),original(v.properties),hidden(h){
 auto ent=original;auto& dlg=*this;
				// flags
				dlg.m_isdir    =	(ent.flags & HFS_ISDIR) != 0;
				dlg.m_islocked =	(ent.flags & HFS_ISLOCKED) != 0;

				// common
				ent.name[HFS_MAX_FLEN] = 0;
				mac_to_pc_charset( (unsigned char *)ent.name );

				dlg.m_name = CString( ent.name );
				dlg.m_nodeid	= ent.cnid;
				dlg.m_parentid = ent.parid;
				dlg.m_backup = ent.bkdate;
				dlg.m_created = ent.crdate;
				dlg.m_modified = ent.mddate;

				// finder location
				dlg.m_x = ent.fdlocation.h;
				dlg.m_y = ent.fdlocation.v;

				// flags
				if(dlg.m_isdir) {
					// file
					dlg.m_rsize = 0;
					dlg.m_dsize = 0;
					dlg.m_creator = "";
					dlg.m_type =  "";

					// folder
					dlg.m_valence		= ent.u.dir.valence;
					dlg.m_bottom    = ent.u.dir.rect.bottom;
					dlg.m_left			= ent.u.dir.rect.left;
					dlg.m_rifht 		= ent.u.dir.rect.right;
					dlg.m_top 			= ent.u.dir.rect.top;
				} else {
					// file
					dlg.m_rsize = ent.u.file.rsize;
					dlg.m_dsize = ent.u.file.dsize;
					ent.u.file.creator[4] = 0;
					ent.u.file.type[4] = 0;
					dlg.m_creator = CString( ent.u.file.creator );
					dlg.m_type =  CString( ent.u.file.type );

					// folder
					dlg.m_valence		= 0;
					dlg.m_bottom    = 0;
					dlg.m_left			= 0;
					dlg.m_rifht 		= 0;
					dlg.m_top 			= 0;
				}

				// fdflags
				dlg.m_isindesktop = (ent.fdflags & HFS_FNDR_ISONDESK) != 0;
				dlg.m_hasbeeninited = (ent.fdflags & HFS_FNDR_HASBEENINITED) != 0;
				dlg.m_isinvisible  = (ent.fdflags & HFS_FNDR_ISINVISIBLE) != 0;
				dlg.m_hasbundle = (ent.fdflags & HFS_FNDR_HASBUNDLE) != 0;
				dlg.m_hascolors = (ent.fdflags & HFS_FNDR_COLOR) != 0;
				dlg.m_hascustomicons = (ent.fdflags & HFS_FNDR_HASCUSTOMICON) != 0;
				dlg.m_hasnoinits = (ent.fdflags & HFS_FNDR_HASNOINITS) != 0;
				dlg.m_isalias = (ent.fdflags & HFS_FNDR_ISALIAS) != 0;
				dlg.m_isnamelocked = (ent.fdflags & HFS_FNDR_NAMELOCKED) != 0;
				dlg.m_isshared = (ent.fdflags & HFS_FNDR_ISSHARED) != 0;
				dlg.m_isstationery = (ent.fdflags & HFS_FNDR_ISSTATIONERY) != 0;
				dlg.m_reqswitchlaunch = (ent.fdflags & HFS_FNDR_REQUIRESSWITCHLAUNCH) != 0;
				dlg.m_reserved = (ent.fdflags & HFS_FNDR_RESERVED) != 0;
				dlg.m_reservedcolors = (ent.fdflags & HFS_FNDR_COLORRESERVED) != 0;

 }
 BOOL OnInitDialog()override{auto result=CAskProperties::OnInitDialog();return result;}
 void OnOK()override{
 if(!UpdateData(TRUE))return;auto ent=original;auto& dlg=*this;
					// flags
					if(dlg.m_isdir)
						ent.flags |= HFS_ISDIR;
					else
						ent.flags &= ~HFS_ISDIR;
					if(dlg.m_islocked)
						ent.flags |= HFS_ISLOCKED;
					else
						ent.flags &= ~HFS_ISLOCKED;

					if(dlg.m_isdir) {
						// folder
						ent.u.dir.valence = dlg.m_valence;
						ent.u.dir.rect.bottom = dlg.m_bottom;
						ent.u.dir.rect.left = dlg.m_left;
						ent.u.dir.rect.right = dlg.m_rifht;
						ent.u.dir.rect.top = dlg.m_top;
					} else {
						// file
						ent.u.file.rsize = dlg.m_rsize;
						ent.u.file.dsize = dlg.m_dsize;
						strncpy( ent.u.file.type, dlg.m_type.GetBuffer(100), 4 );
						ent.u.file.type[4] = 0;
						strncpy( ent.u.file.creator, dlg.m_creator.GetBuffer(100), 4 );
						ent.u.file.creator[4] = 0;
					}

					// common
					strncpy( ent.name, dlg.m_name.GetBuffer(100), HFS_MAX_FLEN );
					ent.name[HFS_MAX_FLEN] = 0;
					ent.cnid = dlg.m_nodeid;
					ent.parid = dlg.m_parentid;
					ent.bkdate = dlg.m_backup;
					ent.crdate = dlg.m_created;
					ent.mddate = dlg.m_modified;

					// finder location
					ent.fdlocation.h = dlg.m_x;
					ent.fdlocation.v = dlg.m_y;

					// fdflags
					if(dlg.m_isindesktop)
						ent.fdflags |= HFS_FNDR_ISONDESK;
					else
						ent.fdflags &= ~HFS_FNDR_ISONDESK;
					if(dlg.m_hasbeeninited)
						ent.fdflags |= HFS_FNDR_HASBEENINITED;
					else
						ent.fdflags &= ~HFS_FNDR_HASBEENINITED;
					if(dlg.m_isinvisible)
						ent.fdflags |= HFS_FNDR_ISINVISIBLE;
					else
						ent.fdflags &= ~HFS_FNDR_ISINVISIBLE;
					if(dlg.m_hasbundle)
						ent.fdflags |= HFS_FNDR_HASBUNDLE;
					else
						ent.fdflags &= ~HFS_FNDR_HASBUNDLE;
					if(dlg.m_hascolors)
						ent.fdflags |= HFS_FNDR_COLOR;
					else
						ent.fdflags &= ~HFS_FNDR_COLOR;
					if(dlg.m_hascustomicons)
						ent.fdflags |= HFS_FNDR_HASCUSTOMICON;
					else
						ent.fdflags &= ~HFS_FNDR_HASCUSTOMICON;
					if(dlg.m_hasnoinits)
						ent.fdflags |= HFS_FNDR_HASNOINITS;
					else
						ent.fdflags &= ~HFS_FNDR_HASNOINITS;
					if(dlg.m_isalias)
						ent.fdflags |= HFS_FNDR_ISALIAS;
					else
						ent.fdflags &= ~HFS_FNDR_ISALIAS;
					if(dlg.m_isnamelocked)
						ent.fdflags |= HFS_FNDR_NAMELOCKED;
					else
						ent.fdflags &= ~HFS_FNDR_NAMELOCKED;
					if(dlg.m_isshared)
						ent.fdflags |= HFS_FNDR_ISSHARED;
					else
						ent.fdflags &= ~HFS_FNDR_ISSHARED;
					if(dlg.m_isstationery)
						ent.fdflags |= HFS_FNDR_ISSTATIONERY;
					else
						ent.fdflags &= ~HFS_FNDR_ISSTATIONERY;
					if(dlg.m_reqswitchlaunch)
						ent.fdflags |= HFS_FNDR_REQUIRESSWITCHLAUNCH;
					else
						ent.fdflags &= ~HFS_FNDR_REQUIRESSWITCHLAUNCH;
					if(dlg.m_reserved)
						ent.fdflags |= HFS_FNDR_RESERVED;
					else
						ent.fdflags &= ~HFS_FNDR_RESERVED;
					if(dlg.m_reservedcolors)
						ent.fdflags |= HFS_FNDR_COLORRESERVED;
					else
						ent.fdflags &= ~HFS_FNDR_COLORRESERVED;

 pc_to_mac_charset((unsigned char*)ent.name);
 FileOperations::EditClassicProperties request{source,FileOperations::ClassicPropertyEdit{original,ent}};
 if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();
 }
 void PostNcDestroy()override{source={};original={};CAskProperties::PostNcDestroy();}
 void OnCancel()override{DestroyWindow();}
};
void Present(FileOperations::Location source,const FileOperations::Completion& value,bool hidden,HWND anchor){
 CollectClosed();
 std::unique_ptr<CDialog> dialog;
 UINT resource=IDD_METADATA_SUMMARY;
 if(value.capabilities.basicMetadata&&!value.entry.directory&&!IsApfs(source)){
  resource=IDD_MFS_PROPERTIES;
  dialog.reset(new MfsEditor(std::move(source),value,hidden));
 }else if(value.classicProperties&&value.capabilities.CanEditClassicProperties()){
  resource=IDD_HFS_ITEM_PROPERTIES;
  dialog.reset(new Editor(std::move(source),value,hidden));
 }else dialog.reset(new Summary(std::move(source),value,hidden));
 if(!dialog->Create(resource,CWnd::GetDesktopWindow()))throw HfsPlus::Error("Cannot create Properties window");NativeDpi::CenterPopup(dialog->GetSafeHwnd(),anchor);if(!hidden)dialog->ShowWindow(SW_SHOWNOACTIVATE);windows.push_back(std::move(dialog));
}
}
bool Show(FileOperations::Location source,CWnd* owner){
 bool hidden=false;
#ifdef HFV_TESTING
 hidden=owner&&!owner->IsWindowVisible();
#endif
 FileOperations::InspectProperties request{source};
 const HWND anchor=owner?owner->GetSafeHwnd():nullptr;
 return BackgroundCopies::Start({request},owner,true,{},[source,hidden,anchor](const FileOperations::Completion& value){Present(source,value,hidden,anchor);});
}
void Shutdown(){for(auto& window:windows)if(IsWindow(window->GetSafeHwnd()))window->DestroyWindow();CollectClosed();}
#ifdef HFV_TESTING
bool ImportValue(HWND window,FileOperations::Location file,const std::u16string& name){auto dialog=dynamic_cast<AttributesDialog*>(CWnd::FromHandlePermanent(window));if(!dialog)return false;DialogDispatch active(*dialog);return dialog->BeginImport(std::move(file),name);}
std::string LastError(){return lastError;}
unsigned CloseDuringDispatch(HWND window){nestedRetained=0;SendMessage(window,WM_APP+173,0,0);auto count=nestedRetained;CollectClosed();return count;}
HWND Window(){for(auto at=windows.rbegin();at!=windows.rend();++at)if(IsWindow((*at)->GetSafeHwnd()))return (*at)->GetSafeHwnd();return nullptr;}
#endif
}
