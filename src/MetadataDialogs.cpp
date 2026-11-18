// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "AskProperties.h"
#include "MetadataDialogs.h"
#include "BackgroundCopies.h"
#include "BrowserPresentation.h"
#include "charset.h"
#include "HfsPlusDisk.h"
#include "HfsPlusDialogs.h"
#include <algorithm>
namespace MetadataDialogs {
namespace {
std::vector<std::unique_ptr<CDialog>> windows;

struct AttributeState {FileOperations::Location source;HfsPlus::Attributes original,desired;HWND window=nullptr;bool writable=false;};
std::wstring Text(HWND window,int id){auto control=GetDlgItem(window,id);int n=GetWindowTextLengthW(control);std::wstring text(n+1,0);GetWindowTextW(control,&text[0],n+1);text.resize(n);return text;}
struct AttributesDialog:CDialog {
 std::shared_ptr<AttributeState> state=std::make_shared<AttributeState>();bool hidden,hexEditable=true;uint64_t pageOffset=0,pageLength=0;std::u16string pageName;
 AttributesDialog(FileOperations::Location source,const FileOperations::Completion& value,bool h):CDialog(IDD_EXTENDED_ATTRIBUTES,CWnd::GetDesktopWindow()),hidden(h){state->source=std::move(source);state->desired=state->original=value.attributes;state->writable=value.writable&&!value.entry.protectedFile&&value.entry.id!=2;}
 std::u16string Name(){return MacBrowser::U16(Text(m_hWnd,IDC_ATTR_NAME));}
 void Refresh(){SendDlgItemMessage(IDC_ATTR_LIST,LB_RESETCONTENT);for(const auto& a:state->desired){auto text=MacBrowser::Wide(a.first)+L"  ("+std::to_wstring(a.second.size())+L" bytes)";SendDlgItemMessageW(m_hWnd,IDC_ATTR_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}}
 void Clear(){SetDlgItemText(IDC_ATTR_OFFSET,"0");GetDlgItem(IDC_ATTR_GO)->EnableWindow(FALSE);pageOffset=pageLength=0;pageName.clear();GetDlgItem(IDC_ATTR_PREVIOUS)->EnableWindow(FALSE);GetDlgItem(IDC_ATTR_NEXT)->EnableWindow(FALSE);SetDlgItemTextW(m_hWnd,IDC_ATTR_NAME,L"");SetDlgItemTextW(m_hWnd,IDC_ATTR_VALUE,L"");SetDlgItemTextW(m_hWnd,IDC_ATTR_DETAIL,L"Set value stages a change. Apply saves all staged changes.");hexEditable=true;SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETREADONLY,!state->writable);}
 void Page(){GetDlgItem(IDC_ATTR_GO)->EnableWindow(TRUE);SetDlgItemTextW(m_hWnd,IDC_ATTR_OFFSET,std::to_wstring(pageOffset).c_str());auto it=state->desired.find(pageName);if(it==state->desired.end())return;const auto& value=it->second;pageLength=(std::min)(uint64_t(65536),value.size()-pageOffset);HfsPlus::Bytes page(size_t(pageLength),0);value.Read(pageOffset,page.data(),page.size());std::wstring hex;const wchar_t* digits=L"0123456789ABCDEF";for(size_t i=0;i<page.size();++i){auto b=page[i];hex+=digits[b>>4];hex+=digits[b&15];hex+=(i%16==15?L"\r\n":L" ");}
  SetDlgItemTextW(m_hWnd,IDC_ATTR_VALUE,hex.c_str());SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETREADONLY,!state->writable);GetDlgItem(IDC_ATTR_PREVIOUS)->EnableWindow(pageOffset!=0);GetDlgItem(IDC_ATTR_NEXT)->EnableWindow(pageOffset+pageLength<value.size());
  auto detail=L"Bytes "+std::to_wstring(pageOffset)+L" - "+std::to_wstring(pageOffset+pageLength)+L" of "+std::to_wstring(value.size())+L". Set value stages this page; Apply saves staged changes.";SetDlgItemTextW(m_hWnd,IDC_ATTR_DETAIL,detail.c_str());
 }
 void Selected(){int selected=int(SendDlgItemMessage(IDC_ATTR_LIST,LB_GETCURSEL));if(selected<0||size_t(selected)>=state->desired.size())return;auto it=state->desired.begin();std::advance(it,selected);pageName=it->first;pageOffset=0;SetDlgItemTextW(m_hWnd,IDC_ATTR_NAME,MacBrowser::Wide(pageName).c_str());Page();}
 void StageValue(){auto text=Text(m_hWnd,IDC_ATTR_VALUE);HfsPlus::Bytes bytes;int half=-1;for(auto c:text){if(iswspace(c))continue;int n=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:-1;if(n<0)throw HfsPlus::Error("Enter hexadecimal byte pairs (00 through FF)");if(half<0)half=n;else{bytes.push_back(uint8_t(half*16+n));half=-1;}}if(half>=0)throw HfsPlus::Error("A hexadecimal byte needs two digits");auto next=state->desired;auto name=Name();if(!pageName.empty()&&state->desired.count(pageName)&&state->desired.at(pageName).size()>65536&&name!=pageName)throw HfsPlus::Error("Keep the selected large attribute name unchanged; use New or Import value to create another attribute");if(name==pageName&&next.count(name)&&next.at(name).size()>65536){if(bytes.size()!=pageLength)throw HfsPlus::Error("Keep this page length unchanged; Import value can replace the entire attribute");next[name]=next.at(name).Patch(pageOffset,bytes);}else next[name]=std::move(bytes);HfsPlus::ValidateAttributes(next);state->desired=std::move(next);Refresh();}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();state->window=m_hWnd;SetDlgItemTextW(m_hWnd,IDC_CP_PHASE,state->writable?L"Named attributes and com.apple.system.Security (Mac ACL)":L"Read-only attributes and Mac ACL");SendDlgItemMessage(IDC_ATTR_VALUE,EM_SETLIMITTEXT,262144);for(int id:{IDC_ATTR_SET,IDC_ATTR_REMOVE,IDC_ATTR_IMPORT,IDC_ATTR_NEW,IDOK})GetDlgItem(id)->EnableWindow(state->writable);Refresh();Clear();return TRUE;}
 void PostNcDestroy()override{state->window=nullptr;CDialog::PostNcDestroy();}
 BOOL OnCommand(WPARAM wp,LPARAM lp)override{
  int id=LOWORD(wp);try{
   if(id==IDC_ATTR_REFRESH){Refresh();Clear();return TRUE;}
   if(id==IDC_ATTR_LIST&&HIWORD(wp)==LBN_SELCHANGE){auto index=SendDlgItemMessage(IDC_ATTR_LIST,LB_GETCURSEL);if(state->writable&&!pageName.empty())StageValue();SendDlgItemMessage(IDC_ATTR_LIST,LB_SETCURSEL,index);Selected();return TRUE;}
   if(id==IDC_ATTR_NEW){Clear();return TRUE;}
   if(id==IDC_ATTR_GO){auto text=Text(m_hWnd,IDC_ATTR_OFFSET);uint64_t offset=0;if(text.empty())throw HfsPlus::Error("Enter a decimal byte offset");for(auto c:text){if(c<L'0'||c>L'9'||offset>(UINT64_MAX-(c-L'0'))/10)throw HfsPlus::Error("Invalid byte offset");offset=offset*10+(c-L'0');}if(!state->desired.count(pageName)||(offset&&offset>=state->desired.at(pageName).size()))throw HfsPlus::Error("Byte offset lies outside the attribute");if(state->writable)StageValue();pageOffset=offset/65536*65536;Page();return TRUE;}
   if(id==IDC_ATTR_PREVIOUS||id==IDC_ATTR_NEXT){if(state->writable)StageValue();if(id==IDC_ATTR_PREVIOUS&&pageOffset)pageOffset-=65536;else if(id==IDC_ATTR_NEXT&&state->desired.count(pageName)&&pageOffset+pageLength<state->desired.at(pageName).size())pageOffset+=pageLength;Page();return TRUE;}
   if(id==IDC_ATTR_SET&&state->writable){StageValue();return TRUE;}
   if(id==IDC_ATTR_REMOVE&&state->writable){state->desired.erase(Name());Refresh();Clear();return TRUE;}
   if(id==IDC_ATTR_IMPORT&&state->writable){auto name=Name();if(name.empty())throw HfsPlus::Error("Enter the attribute name first");CFileDialog picker(TRUE,nullptr,nullptr,OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR,"Binary files (*.*)|*.*||",this);if(picker.DoModal()!=IDOK)return TRUE;FileOperations::Request request;request.kind=FileOperations::Kind::ReadAttributeFile;request.name=name;FileOperations::Location file;file.SetWindows(VolumeCoord::Wide((LPCSTR)picker.GetPathName()));request.sources={file};auto model=state;
    BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true,{},[model](const FileOperations::Completion& value){if(!model->window)return;try{auto next=model->desired;for(auto& a:value.attributes)next[a.first]=a.second;HfsPlus::ValidateAttributes(next);model->desired=std::move(next);::SendMessage(model->window,WM_COMMAND,IDC_ATTR_REFRESH,0);}catch(const std::exception& e){::MessageBoxA(model->window,e.what(),"Invalid attribute value",MB_OK|MB_ICONERROR);}});return TRUE;}
   if(id==IDC_ATTR_EXPORT){auto name=Name();if(!state->desired.count(name))throw HfsPlus::Error("Select an attribute first");CFileDialog picker(FALSE,"bin",nullptr,OFN_HIDEREADONLY|OFN_NOCHANGEDIR,"Binary files (*.bin)|*.bin|All files (*.*)|*.*||",this);if(picker.DoModal()!=IDOK)return TRUE;FileOperations::Request request;request.kind=FileOperations::Kind::ExportAttribute;request.name=name;request.attributes={{name,state->desired.at(name)}};request.target.SetWindows(VolumeCoord::Wide((LPCSTR)picker.GetPathName()));BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true);return TRUE;}
  }catch(const std::exception& error){::MessageBoxA(m_hWnd,error.what(),"Extended attributes",MB_OK|MB_ICONERROR);return TRUE;}
  return CDialog::OnCommand(wp,lp);
 }
 void OnOK()override{if(!state->writable)return;FileOperations::Request request;request.kind=FileOperations::Kind::SetAttributes;request.sources={state->source};request.originalAttributes=state->original;request.attributes=state->desired;if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();}
 void OnCancel()override{DestroyWindow();}
};
void PresentAttributes(FileOperations::Location source,const FileOperations::Completion& value,bool hidden){auto dialog=std::make_unique<AttributesDialog>(std::move(source),value,hidden);if(!dialog->Create(IDD_EXTENDED_ATTRIBUTES,CWnd::GetDesktopWindow()))throw HfsPlus::Error("Cannot create attribute editor");if(!hidden)dialog->ShowWindow(SW_SHOWNOACTIVATE);windows.push_back(std::move(dialog));}
struct Summary:CDialog {
 FileOperations::Completion value;FileOperations::Location source;bool hidden;
 Summary(FileOperations::Location s,const FileOperations::Completion& v,bool h):CDialog(IDD_METADATA_SUMMARY,CWnd::GetDesktopWindow()),value(v),source(std::move(s)),hidden(h){}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();auto e=value.entry;SetDlgItemTextW(m_hWnd,IDC_CP_PHASE,MacBrowser::Wide(e.name).c_str());
 auto description=BrowserPresentation::FromUtf8(value.description)+(value.writable?L"":L" (read-only)");SetDlgItemTextW(m_hWnd,IDC_CP_FILE,description.c_str());
 auto sizes=e.directory?L"Folder":L"Data fork: "+std::to_wstring(e.forks[0].size)+L" bytes\r\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";if(e.hardLink)sizes+=L"\r\nHard links: "+std::to_wstring(e.linkCount);SetDlgItemTextW(m_hWnd,IDC_CP_STATUS,sizes.c_str());GetDlgItem(IDC_ATTR_OPEN)->ShowWindow(value.supportsAttributes?SW_SHOW:SW_HIDE);
 bool compression=value.supportsCompression&&!e.directory;GetDlgItem(IDC_FILE_COMPRESSION)->ShowWindow(compression?SW_SHOW:SW_HIDE);GetDlgItem(IDC_FILE_COMPRESSION)->EnableWindow(compression&&value.writable&&!e.protectedFile&&e.readError.empty());SetDlgItemText(IDC_FILE_COMPRESSION,e.compressed?"Decompress":"Compress");
 for(int id:{IDC_COMPRESSION_LABEL,IDC_COMPRESSION_METHOD})GetDlgItem(id)->ShowWindow(compression?SW_SHOW:SW_HIDE);
 auto combo=static_cast<CComboBox*>(GetDlgItem(IDC_COMPRESSION_METHOD));
 for(const char* label:{"Uncompressed","zlib","LZVN","LZFSE","LZBITMAP","Stored (no size reduction)","Legacy stored (inline, up to 3786 bytes)"})combo->AddString(label);
 combo->SetCurSel(e.compressed?0:1);combo->EnableWindow(compression&&value.writable&&!e.protectedFile&&e.readError.empty());return TRUE;}
 BOOL OnCommand(WPARAM wp,LPARAM lp)override{if(LOWORD(wp)==IDC_COMPRESSION_METHOD&&HIWORD(wp)==CBN_SELCHANGE){auto selected=SendDlgItemMessage(IDC_COMPRESSION_METHOD,CB_GETCURSEL);SetDlgItemText(IDC_FILE_COMPRESSION,selected?"Compress":"Decompress");return TRUE;}if(LOWORD(wp)==IDC_ATTR_OPEN){PresentAttributes(source,value,hidden);return TRUE;}
 if(LOWORD(wp)==IDC_FILE_COMPRESSION){FileOperations::Request request;request.kind=FileOperations::Kind::SetCompression;request.sources={source};request.originalMetadata=value.entry;request.originalAttributes=value.attributes;using Mode=HfsPlus::Volume::CompressionMode;const Mode methods[]={Mode::Uncompressed,Mode::Zlib,Mode::Lzvn,Mode::Lzfse,Mode::Lzbitmap,Mode::Stored,Mode::StoredInline};auto selected=SendDlgItemMessage(IDC_COMPRESSION_METHOD,CB_GETCURSEL);if(selected<0||selected>=ARRAYSIZE(methods))return TRUE;request.compressionMethod=methods[selected];request.compressed=selected!=0;if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();return TRUE;}return CDialog::OnCommand(wp,lp);}
 void OnOK()override{DestroyWindow();}void OnCancel()override{DestroyWindow();}
};

struct MfsEditor:CDialog {
 FileOperations::Location source;MacBrowser::Entry original;bool hidden,writable;
 MfsEditor(FileOperations::Location s,const FileOperations::Completion& value,bool h):CDialog(IDD_MFS_PROPERTIES,CWnd::GetDesktopWindow()),source(std::move(s)),original(value.entry),hidden(h),writable(value.writable){}
 void SetDate(int id,uint32_t value){ULARGE_INTEGER ticks;ticks.QuadPart=(uint64_t(value)+9561628800ULL)*10000000ULL;FILETIME file{ticks.LowPart,ticks.HighPart};SYSTEMTIME time{};FileTimeToSystemTime(&file,&time);SendDlgItemMessageW(m_hWnd,id,DTM_SETFORMATW,0,reinterpret_cast<LPARAM>(L"yyyy-MM-dd HH:mm:ss"));SendDlgItemMessage(id,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&time));}
 uint32_t Date(int id){SYSTEMTIME time{};if(SendDlgItemMessage(id,DTM_GETSYSTEMTIME,0,reinterpret_cast<LPARAM>(&time))!=GDT_VALID)throw HfsPlus::Error("Choose a valid date");FILETIME file{};if(!SystemTimeToFileTime(&time,&file))throw HfsPlus::Error("Invalid date");ULARGE_INTEGER ticks;ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;auto seconds=ticks.QuadPart/10000000ULL;if(seconds<9561628800ULL||seconds-9561628800ULL>UINT32_MAX)throw HfsPlus::Error("MFS dates must be between 1904 and February 2040");return uint32_t(seconds-9561628800ULL);}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();HfvUnicodeNameEdit(m_hWnd,IDC_HPLUS_NAME);SetDlgItemTextW(m_hWnd,IDC_HPLUS_NAME,MacBrowser::Wide(original.name).c_str());SendDlgItemMessage(IDC_HPLUS_NAME,EM_SETLIMITTEXT,255);SetDlgItemInt(IDC_MFS_VERSION,original.version,FALSE);CheckDlgButton(IDC_MFS_LOCKED,original.protectedFile?BST_CHECKED:BST_UNCHECKED);SetDate(IDC_MFS_CREATED,original.created);SetDate(IDC_MFS_MODIFIED,original.modified);std::wstring hex;for(unsigned i=0;i<16;++i){wchar_t b[4];swprintf_s(b,L"%02X ",original.finder[i]);hex+=b;}SetDlgItemTextW(m_hWnd,IDC_MFS_FINDER,hex.c_str());for(int id:{IDC_HPLUS_NAME,IDC_MFS_VERSION,IDC_MFS_LOCKED,IDC_MFS_CREATED,IDC_MFS_MODIFIED,IDC_MFS_FINDER,IDOK})GetDlgItem(id)->EnableWindow(writable);return TRUE;}
 void OnOK()override{if(!writable)return;try{auto entry=original;std::string encoded,error;if(!MfsImage::EncodeName(Text(m_hWnd,IDC_HPLUS_NAME),encoded,error))throw HfsPlus::Error(error);entry.name=MacBrowser::U16(MfsDisplayName(encoded));auto text=Text(m_hWnd,IDC_MFS_VERSION);wchar_t* end=nullptr;auto version=wcstoul(text.c_str(),&end,10);if(text.empty()||*end||version>255)throw HfsPlus::Error("MFS file version must be 0-255");entry.version=version;entry.protectedFile=IsDlgButtonChecked(IDC_MFS_LOCKED)==BST_CHECKED;entry.created=Date(IDC_MFS_CREATED);entry.modified=Date(IDC_MFS_MODIFIED);entry.datesValid=true;
  auto hex=Text(m_hWnd,IDC_MFS_FINDER);HfsPlus::Bytes bytes;int half=-1;for(auto c:hex){if(iswspace(c))continue;int n=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:-1;if(n<0)throw HfsPlus::Error("Finder information requires hexadecimal bytes");if(half<0)half=n;else{bytes.push_back(uint8_t(half*16+n));half=-1;}}if(half>=0||bytes.size()!=16)throw HfsPlus::Error("Finder information must contain exactly 16 bytes");std::copy(bytes.begin(),bytes.end(),entry.finder.begin());FileOperations::Request request;request.kind=FileOperations::Kind::SetBasicMetadata;request.sources={source};request.originalMetadata=original;request.metadata=std::move(entry);if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();
 }catch(const std::exception& error){::MessageBoxA(m_hWnd,error.what(),"MFS properties",MB_OK|MB_ICONERROR);}}
 void OnCancel()override{DestroyWindow();}
};
struct Editor:CAskProperties {
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
 FileOperations::Request request;request.kind=FileOperations::Kind::SetProperties;request.sources={source};request.originalProperties=original;request.properties=ent;
 if(BackgroundCopies::Start({request},hidden?this:AfxGetMainWnd(),true))DestroyWindow();
 }
 void OnCancel()override{DestroyWindow();}
};
void Present(FileOperations::Location source,const FileOperations::Completion& value,bool hidden){
 windows.erase(std::remove_if(windows.begin(),windows.end(),[](const std::unique_ptr<CDialog>& window){return !IsWindow(window->GetSafeHwnd());}),windows.end());
 std::unique_ptr<CDialog> dialog;
 if(value.basicMetadata&&!value.entry.directory)dialog.reset(new MfsEditor(std::move(source),value,hidden));else if(value.classicProperties&&value.writable)dialog.reset(new Editor(std::move(source),value,hidden));else dialog.reset(new Summary(std::move(source),value,hidden));
 UINT resource=value.basicMetadata&&!value.entry.directory?IDD_MFS_PROPERTIES:value.classicProperties&&value.writable?IDD_HFS_ITEM_PROPERTIES:IDD_METADATA_SUMMARY;
 if(!dialog->Create(resource,CWnd::GetDesktopWindow()))throw HfsPlus::Error("Cannot create Properties window");if(!hidden)dialog->ShowWindow(SW_SHOWNOACTIVATE);windows.push_back(std::move(dialog));
}
}
bool Show(FileOperations::Location source,CWnd* owner){
 bool hidden=false;
#ifdef HFV_TESTING
 hidden=owner&&!owner->IsWindowVisible();
#endif
 FileOperations::Request request;request.kind=FileOperations::Kind::Inspect;request.sources={source};
 return BackgroundCopies::Start({request},owner,true,{},[source,hidden](const FileOperations::Completion& value){Present(source,value,hidden);});
}
void Shutdown(){for(auto& window:windows)if(IsWindow(window->GetSafeHwnd()))window->DestroyWindow();windows.clear();}
#ifdef HFV_TESTING
HWND Window(){for(auto at=windows.rbegin();at!=windows.rend();++at)if(IsWindow((*at)->GetSafeHwnd()))return (*at)->GetSafeHwnd();return nullptr;}
#endif
}
