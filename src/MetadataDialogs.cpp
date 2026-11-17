// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "AskProperties.h"
#include "MetadataDialogs.h"
#include "BackgroundCopies.h"
#include "BrowserPresentation.h"
#include "charset.h"
#include <algorithm>
namespace MetadataDialogs {
namespace {
std::vector<std::unique_ptr<CDialog>> windows;
struct Summary:CDialog {
 FileOperations::Completion value;bool hidden;
 Summary(const FileOperations::Completion& v,bool h):CDialog(IDD_METADATA_SUMMARY,CWnd::GetDesktopWindow()),value(v),hidden(h){}
 BOOL OnInitDialog()override{CDialog::OnInitDialog();auto e=value.entry;SetDlgItemTextW(m_hWnd,IDC_CP_PHASE,MacBrowser::Wide(e.name).c_str());
 auto description=BrowserPresentation::FromUtf8(value.description)+(value.writable?L"":L" (read-only)");SetDlgItemTextW(m_hWnd,IDC_CP_FILE,description.c_str());
 auto sizes=e.directory?L"Folder":L"Data fork: "+std::to_wstring(e.forks[0].size)+L" bytes\r\nResource fork: "+std::to_wstring(e.forks[1].size)+L" bytes";SetDlgItemTextW(m_hWnd,IDC_CP_STATUS,sizes.c_str());return TRUE;}
 void OnOK()override{DestroyWindow();}void OnCancel()override{DestroyWindow();}
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
 if(value.classicProperties&&value.writable)dialog.reset(new Editor(std::move(source),value,hidden));else dialog.reset(new Summary(value,hidden));
 UINT resource=value.classicProperties&&value.writable?IDD_HFS_ITEM_PROPERTIES:IDD_METADATA_SUMMARY;
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
