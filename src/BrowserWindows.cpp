// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BackgroundCopies.h"
#include "HFVExplorer.h"
#include "HFVExplorerView.h"
#include "BrowserSession.h"
#include "DeviceWait.h"
#include <algorithm>
extern "C" {
#include "hfs/libhfs/hfs.h"
#include "hfs/interface.h"
}

namespace {
// MFC's multi-document collection also supports independent top-level frames.
// There is deliberately no MDI parent and no document shared between frames.
class BrowserTemplate : public CMultiDocTemplate {
public:
 BrowserTemplate(CRuntimeClass* doc,CRuntimeClass* frame):CMultiDocTemplate(IDR_MAINFRAME,doc,frame,RUNTIME_CLASS(CHFVExplorerView)){}
 CDocument* OpenDocumentFile(LPCTSTR path,BOOL visible=TRUE)override{return OpenDocumentFile(path,TRUE,visible);}
 CDocument* OpenDocumentFile(LPCTSTR path,BOOL addToMru,BOOL visible)override{
  auto doc=CMultiDocTemplate::OpenDocumentFile(path,addToMru,FALSE);
  if(!doc)return nullptr;
  auto frame=static_cast<CMainFrame*>(static_cast<CHFVExplorerDoc*>(doc)->GetBrowserSession().Owner());
  static_cast<CHFVExplorerApp*>(AfxGetApp())->RegisterBrowser(frame);
  frame->DragAcceptFiles();
  InitialUpdateFrame(frame,doc,visible);
  return doc;
 }
};
}
void CHFVExplorerApp::InstallBrowserTemplate(CRuntimeClass* doc,CRuntimeClass* frame){
 if(m_browserTemplate)return;
 m_browserTemplate=new BrowserTemplate(doc?doc:RUNTIME_CLASS(CHFVExplorerDoc),frame?frame:RUNTIME_CLASS(CMainFrame));
 AddDocTemplate(m_browserTemplate);
}
CMainFrame* CHFVExplorerApp::CreateBrowser(BOOL visible){
 if(BackgroundCopies::Active())return nullptr;
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return nullptr;
 InstallBrowserTemplate();
 auto doc=static_cast<CHFVExplorerDoc*>(m_browserTemplate->OpenDocumentFile(nullptr,visible));
 return doc?static_cast<CMainFrame*>(doc->GetBrowserSession().Owner()):nullptr;
}
void CHFVExplorerApp::RegisterBrowser(CMainFrame* frame){
 if(std::find(m_browsers.begin(),m_browsers.end(),frame)==m_browsers.end())m_browsers.push_back(frame);
 ActivateBrowser(frame);
}
void CHFVExplorerApp::ActivateBrowser(CMainFrame* frame){
 if(std::find(m_browsers.begin(),m_browsers.end(),frame)==m_browsers.end())return;
 auto& session=frame->GetBrowserSession();
 m_pMainWnd=m_main=frame;m_doc=session.Document();m_tree=session.Tree();m_list=session.List();m_pre=session.Preview();m_pStatusBar=session.Status();
 // Kept only for old launcher utilities. Browser operations use their session.
 if(!BackgroundCopies::Active())HFSIFACE_set_error_window(frame->GetSafeHwnd());
}
void CHFVExplorerApp::ForgetBrowser(CMainFrame* frame){
 auto found=std::find(m_browsers.begin(),m_browsers.end(),frame);if(found==m_browsers.end())return;
 m_browsers.erase(found);
 if(m_main!=frame&&m_pMainWnd!=frame)return;
 if(!m_browsers.empty()){ActivateBrowser(m_browsers.back());return;}
 m_main=nullptr;m_doc=nullptr;m_tree=nullptr;m_list=nullptr;m_pre=nullptr;m_pStatusBar=nullptr;m_wndSplitter=nullptr;m_wndSplitter2=nullptr;
 if(!BackgroundCopies::Active())HFSIFACE_set_error_window(nullptr);
 // Leave m_pMainWnd until WM_NCDESTROY: MFC posts WM_QUIT for the last frame.
}
void CHFVExplorerApp::OnNewBrowser(){CreateBrowser();}
void CHFVExplorerApp::OnUpdateNewBrowser(CCmdUI* ui){ui->Enable(!BackgroundCopies::Active()&&!StorageUi::busy()&&!StorageUi::state().sessionOpen);}
CDocument* CHFVExplorerApp::OpenDocumentFile(LPCTSTR path){
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return nullptr;
 if(m_main){auto doc=m_main->GetBrowserSession().Document();return doc->OnOpenDocument(path)?doc:nullptr;}
 InstallBrowserTemplate();return m_browserTemplate->OpenDocumentFile(path);
}
void CHFVExplorerApp::OnExitBrowsers(){
 if(BackgroundCopies::Active()){BackgroundCopies::CancelAll();for(auto frame:m_browsers)frame->SetTimer(9202,100,nullptr);return;}
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return;
 while(!m_browsers.empty()){
  auto frame=m_browsers.back();HWND window=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);
  if(IsWindow(window))break; // A launched-document/save prompt cancelled closing.
 }
}
