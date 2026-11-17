// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "HfsPlusDisk.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "utils.h"
#include <cstdio>
namespace {
CString host;
bool rejectNew=false;
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
class BrowserWindowTestDoc:public CHFVExplorerDoc {
 DECLARE_DYNCREATE(BrowserWindowTestDoc)
public:
 BrowserWindowTestDoc():CHFVExplorerDoc(false){m_fat_count=1;m_fats[0].m_letter=host[0];m_fats[0].m_type=DRIVE_FIXED;m_fats[0].m_volume_name=host;}
 BOOL OnNewDocument()override{if(rejectNew)return FALSE;return CHFVExplorerDoc::OnNewDocument();}
};
IMPLEMENT_DYNCREATE(BrowserWindowTestDoc,CHFVExplorerDoc)
class BrowserWindowTestFrame:public CMainFrame {
 DECLARE_DYNCREATE(BrowserWindowTestFrame)
public:
 void LoadWindowPosition()override{}
 void SaveWindowPosition()override{}
 void ActivateFrame(int)override{} // Real frames, menus and routing; never show UI.
};
IMPLEMENT_DYNCREATE(BrowserWindowTestFrame,CMainFrame)
int Find(BrowserSession& session,const wchar_t* name){
 auto& list=session.List()->GetListCtrl();
 for(int i=0;i<list.GetItemCount();++i){wchar_t text[512]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));if(!wcscmp(text,name))return i;}
 return -1;
}
std::wstring Image(const CString& path,const std::u16string& name){
 int count=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);std::wstring wide(count,L'\0');MultiByteToWideChar(CP_ACP,0,path,-1,&wide[0],count);wide.resize(count-1);
 auto disk=HfsPlus::CreateImage(wide.c_str(),8*1024*1024);HfsPlus::Volume::Format(*disk,name,true);disk.reset();
 auto volume=MacBrowser::OpenImage(wide);volume->MakeDirectory(2,u"One");volume->MakeDirectory(2,u"Two");volume->MakeDirectory(2,u"Cafe \u65e5\u672c");return wide;
}
std::wstring Caption(CWnd* window){wchar_t text[32768]{};GetWindowTextW(window->GetSafeHwnd(),text,ARRAYSIZE(text));return text;}
void NoQuit(){MSG message{};Check(!PeekMessage(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"closing a browser posted WM_QUIT with another window open");}
struct MenuState:CCmdUI{bool enabled=false;void Enable(BOOL value=TRUE)override{enabled=value!=FALSE;}};
}
int RunBrowserWindowsTest(const char* directory){
 if(!CreateDirectoryA(directory,nullptr))return 2;host=directory;FILE* report=fopen(host+"\\browser-windows-report.txt","w");if(!report)return 2;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());InitializeStorageUi();app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;
 int result=0;
 try{
  app.InstallBrowserTemplate(RUNTIME_CLASS(BrowserWindowTestDoc),RUNTIME_CLASS(BrowserWindowTestFrame));
  auto a=app.CreateBrowser(FALSE);Check(a&&app.m_browsers.size()==1&&app.m_pMainWnd==a,"first browser registration");auto& sa=a->GetBrowserSession();
  HWND ah=a->GetSafeHwnd();a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==VolumeCoord::Wide(std::string((LPCSTR)host))+L" \u2014 HFVExplorer","startup Windows folder title was replaced by Untitled");Check(!a->IsWindowVisible(),"test unexpectedly displayed a browser");
  MenuState menu;menu.m_nID=ID_TOOLS_NEW_BROWSER;Check(a->OnCmdMsg(ID_TOOLS_NEW_BROWSER,CN_UPDATE_COMMAND_UI,&menu,nullptr)&&menu.enabled,"New browser has no enabled update handler");
  a->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);Check(app.m_browsers.size()==2,"New browser menu did not create an independent window");
  auto b=app.m_browsers.back();auto& sb=b->GetBrowserSession();HWND bh=b->GetSafeHwnd();
  Check(a!=b&&sa.Document()!=sb.Document()&&sa.Tree()!=sb.Tree()&&sa.List()!=sb.List()&&!b->IsWindowVisible(),"browser windows share document/panes or test UI became visible");
  Check(GetWindow(ah,GW_OWNER)==nullptr&&GetWindow(bh,GW_OWNER)==nullptr,"browser was created as an owned window");
  // Deliberately keep app ownership at B, then route commands to inactive A.
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);
  Check((sa.List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&(sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_ICON,"view command changed the wrong frame");
  a->SendMessage(WM_COMMAND,ID_VIEW_MAC_WINDOW);Check(sa.finderLayout&&!sb.finderLayout,"Finder layout leaked between windows");
  b->SendMessage(WM_COMMAND,ID_HFV_VIEW_LIST);Check(sa.finderLayout&&(sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_LIST,"second frame command used first frame");
  a->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);
  auto firstImage=host+"\\first.dsk",secondImage=host+"\\second.dsk";Image(firstImage,u"First");Image(secondImage,u"Second");
  app.ActivateBrowser(a);Check(app.OpenDocumentFile(firstImage)==sa.Document(),"image open did not target first browser");
  app.ActivateBrowser(b);Check(app.OpenDocumentFile(secondImage)==sb.Document(),"image open did not target second browser");
  Check(sa.MacBrowser().Active()&&sb.MacBrowser().Active(),"opening an image cleared another browser");
  a->OnUpdateFrameTitle(TRUE);b->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","disk captions leaked between frames or MFC restored Untitled");
  Check(sa.MacBrowser().Activate(Find(sa,L"Cafe \u65e5\u672c")),"open Unicode title folder");Check(Caption(a)==L"First:Cafe \u65e5\u672c \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","Unicode folder caption or frame isolation failed");Check(sa.MacBrowser().ShowTestFolder(0,2),"restore title test root");
  sa.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);sb.List()->GetListCtrl().SetItemState(-1,0,LVIS_SELECTED);
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_EDIT_SELECT_ALL);
  Check(sa.List()->GetListCtrl().GetSelectedCount()==sa.List()->GetListCtrl().GetItemCount()&&sa.List()->GetListCtrl().GetItemCount()>=2&&sb.List()->GetListCtrl().GetSelectedCount()==0,"selection command used another window's list");
  Check(sa.MacBrowser().Activate(Find(sa,L"One"))&&Find(sa,L"One")<0&&Find(sb,L"One")>=0,"native folder navigation leaked between real frames");
  app.ActivateBrowser(b);a->SendMessage(WM_COMMAND,ID_VIEW_REFRESH);
  Check(Find(sa,L"One")<0&&Find(sb,L"One")>=0,"refresh command lost a window's current folder");a->OnUpdateFrameTitle(TRUE);Check(Caption(a)==L"First:One \u2014 HFVExplorer"&&Caption(b)==L"Second \u2014 HFVExplorer","refresh changed the current folder caption");
  bool bothDisabled=false;
  auto wait=ShowStorageJob([&](Storage::Job&){for(int n=0;n<100;++n){if(!IsWindowEnabled(ah)&&!IsWindowEnabled(bh)){bothDisabled=true;break;}Sleep(5);}return Storage::Result{};},"Hidden multiwindow operation",true,true,false,a);
  Check(wait.code==0&&bothDisabled&&a->IsWindowEnabled()&&b->IsWindowEnabled(),"storage progress did not disable/restore all browser frames");
  b->EnableWindow(FALSE);
  wait=ShowStorageJob([](Storage::Job&){Sleep(40);return Storage::Result{};},"Hidden prior disabled owner",true,true,false,a);
  Check(wait.code==0&&a->IsWindowEnabled()&&!b->IsWindowEnabled(),"storage completion enabled a previously disabled browser");b->EnableWindow(TRUE);
  // Busy guards apply to every frame, not just the disabled progress owner.
  ++StorageUi::state().active;a->SendMessage(WM_CLOSE);b->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);--StorageUi::state().active;
  Check(IsWindow(ah)&&app.m_browsers.size()==2,"nested close/new command ran during storage access");
  auto previousMain=app.m_pMainWnd;rejectNew=true;Check(app.CreateBrowser(FALSE)==nullptr,"injected failed initialization succeeded");rejectNew=false;
  Check(app.m_browsers.size()==2&&app.m_pMainWnd==previousMain&&IsWindow(ah)&&IsWindow(bh),"failed browser creation damaged existing windows");NoQuit();
  // Exercise the actual resource accelerator, not a duplicate command ID.
  app.ActivateBrowser(a);MSG key{};key.hwnd=ah;key.message=WM_KEYDOWN;key.wParam=VK_RETURN;BYTE prior[256]{},state[256]{};GetKeyboardState(prior);memcpy(state,prior,sizeof state);state[VK_CONTROL]=0x80;SetKeyboardState(state);
  bool translated=TranslateAccelerator(ah,LoadAccelerators(AfxGetResourceHandle(),MAKEINTRESOURCE(IDR_MAINFRAME)),&key)!=0;SetKeyboardState(prior);
  Check(translated&&app.m_browsers.size()==3,"Ctrl+Enter is not wired to New browser");auto c=app.m_browsers.back();HWND ch=c->GetSafeHwnd();
  c->SendMessage(WM_CLOSE);Check(!IsWindow(ch)&&IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==2,"closing the newest window closed another browser");NoQuit();
  // Make the first frame the MFC main window before closing it: this is the
  // case that CFrameWnd's default SDI shutdown used to close every document.
  app.ActivateBrowser(a);a->SendMessage(WM_CLOSE);
  Check(!IsWindow(ah)&&IsWindow(bh)&&app.m_browsers.size()==1&&app.m_pMainWnd==b&&app.m_doc==sb.Document()&&app.m_list==sb.List()&&app.m_tree==sb.Tree(),"first window closure did not transfer ownership to the surviving browser");NoQuit();
  b->SendMessage(WM_COMMAND,ID_HFV_VIEW_DETAILS);Check((sb.List()->GetStyle()&LVS_TYPEMASK)==LVS_REPORT&&sb.MacBrowser().Refresh(),"remaining window is not usable after first closes");
  b->SendMessage(WM_COMMAND,ID_TOOLS_NEW_BROWSER);Check(app.m_browsers.size()==2,"remaining window cannot create another browser");
  // Explicit File/Exit closes the full collection; a title-bar close does not.
  b->SendMessage(WM_COMMAND,ID_APP_EXIT);Check(app.m_browsers.empty()&&!IsWindow(bh)&&app.m_pMainWnd==nullptr&&app.m_main==nullptr&&app.m_doc==nullptr&&app.m_list==nullptr,"File Exit did not close the remaining browser collection");
  MSG quit{};Check(PeekMessage(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)!=FALSE,"last window did not terminate the message loop");
  fprintf(report,"PASS: independent real hidden top-level frames/documents/panes; New browser menu and Ctrl+Enter; view/layout and image-open command routing; per-window disk/folder and Unicode captions, startup and MFC title-update persistence; busy/failed-creation guards; closing newest and first/main windows without WM_QUIT; surviving browser refresh/new commands; explicit File Exit and final-window shutdown. No hardware access or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());result=1;}
 catch(CException* e){char text[512]{};e->GetErrorMessage(text,ARRAYSIZE(text));fprintf(report,"FAIL: MFC exception: %s\n",text);e->Delete();result=1;}
 fclose(report);return result;
}
#endif
