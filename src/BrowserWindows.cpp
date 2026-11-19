// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include <afxadv.h>
#include "BackgroundCopies.h"
#include "HFVExplorer.h"
#include "HFVExplorerView.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "BrowserLifetime.h"
#include "DeviceWait.h"
#include "VolumeCoordinator.h"
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
  // Image requests add their MRU entry after the root listing is published.
  auto doc=CMultiDocTemplate::OpenDocumentFile(path,path?FALSE:addToMru,FALSE);
  if(!doc)return nullptr;
  auto frame=static_cast<CMainFrame*>(static_cast<CHFVExplorerDoc*>(doc)->GetBrowserSession().Owner());
  static_cast<CHFVExplorerApp*>(AfxGetApp())->RegisterBrowser(frame);
  frame->DragAcceptFiles();
  InitialUpdateFrame(frame,doc,visible);
  return doc;
 }
};
}
namespace BrowserLifetime {
namespace {std::vector<std::unique_ptr<CHFVExplorerDoc>> retired;}
void Collect(){
 if(BackgroundCopies::Active())return;
 auto closing=std::move(retired);retired.clear();
 for(auto& document:closing)document->FinishBackgroundClose();
}
void Retire(CHFVExplorerDoc* document){retired.emplace_back(document);Collect();}
#ifdef HFV_TESTING
unsigned RetiredCount(){return unsigned(retired.size());}
#endif
}
void CHFVExplorerApp::InstallBrowserTemplate(CRuntimeClass* doc,CRuntimeClass* frame){
 if(m_browserTemplate)return;
 m_browserTemplate=new BrowserTemplate(doc?doc:RUNTIME_CLASS(CHFVExplorerDoc),frame?frame:RUNTIME_CLASS(CMainFrame));
 AddDocTemplate(m_browserTemplate);
}
CMainFrame* CHFVExplorerApp::CreateBrowser(BOOL visible){
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
 m_pMainWnd=frame;
}
CMainFrame* CHFVExplorerApp::ActiveBrowser() const{
 auto found=std::find(m_browsers.begin(),m_browsers.end(),m_pMainWnd);
 return found==m_browsers.end()?nullptr:*found;
}
void CHFVExplorerApp::ForgetBrowser(CMainFrame* frame){
 auto found=std::find(m_browsers.begin(),m_browsers.end(),frame);if(found==m_browsers.end())return;
 m_browsers.erase(found);
 BackgroundCopies::TransferOwner(frame->GetSafeHwnd(),m_browsers.empty()?nullptr:m_browsers.back()->GetSafeHwnd());
 if(m_pMainWnd!=frame)return;
 if(!m_browsers.empty()){ActivateBrowser(m_browsers.back());return;}
 // Leave m_pMainWnd until WM_NCDESTROY: MFC posts WM_QUIT for the last frame.
}
BrowserNavigation::Request BrowserSession::FolderRequest(HTREEITEM item)
{
 using namespace BrowserNavigation;
 Request request{MacBrowser().TargetFolder(item),Intent::SelectFolder,{}};
 if(request.folder.IsMounted()){
  request.resource=request.folder.Volume()->ResourceKey();
  auto& control=Tree()->GetTreeCtrl();
  for(auto at=item;at;at=control.GetParentItem(at))
   request.catalogRoute.push_back(MacBrowser().TargetFolder(at).Id());
  std::reverse(request.catalogRoute.begin(),request.catalogRoute.end());
 }
 return request;
}

bool BrowserSession::OpenFolderFrom(BrowserSession& source,HTREEITEM item)
{
 return item&&OpenFolderRequest(source,source.FolderRequest(item));
}

bool BrowserSession::OpenFolderRequest(BrowserSession& source,BrowserNavigation::Request request)
{
 if(!document_||!tree_)return false;
 using namespace BrowserNavigation;
 if(request.folder.IsMounted())return MacBrowser().OpenMountedFolderFrom(source.MacBrowser(),request);
 if(request.folder.IsWindows()){
  const auto& path=request.folder.WindowsPath();
  if(path.size()<3||path[1]!=L':')return false;
  if(auto mount=mounts_.FindWindowsDrive(towupper(path[0])-L'A'))
   return document_->NavigateWindowsFolder(mount->address.index,path,false,&request);
 }
 if(request.folder.IsClassic()){
  const auto image=request.folder.ClassicImage();
  const auto attached=mounts_.FindClassicVolume(VolumeCoord::Wide(image));
  int index=attached?attached->address.index:-1;
  if(index<0){
   const bool floppy=request.folder.IsPhysical()&&request.folder.Media()==FileOperations::ClassicMedia::Floppy;
   const bool cd=request.folder.IsPhysical()&&request.folder.Media()==FileOperations::ClassicMedia::Cd;
   index=document_->insert_volume(image.c_str(),floppy,cd,0,0);
   if(index<0)return false;
  }
  auto& volume=document_->m_volumes[index];
  if(!volume.m_rootitem)
   mounts_.SetRoot({BrowserMountInventory::Kind::ClassicVolume,index},
      tree_->initialize_volume(index,image.c_str(),2,volume.m_is_floppy,volume.m_is_cd,0,0));
  // Resolve the selected catalog ID in the new window before publishing it.
  request.intent=Intent::StartupFolder;
  request.folder.expectedResource=VolumeCoord::ImageKey(image);
  request.resource=request.folder.expectedResource;
  return document_->NavigateClassicFolder(index,request.folder.expectedCatalogId,false,&request);
 }
 return false;
}

bool BrowserSession::OpenFolderInNewWindow(HTREEITEM item)
{
 if(!item)return false;
 auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
 try{
  auto request=FolderRequest(item);
  if(request.folder.IsNone())return false;
  auto* frame=app.CreateBrowser();
  if(!frame)return false;
  if(frame->GetBrowserSession().OpenFolderRequest(*this,std::move(request)))return true;
  frame->GetBrowserSession().RejectNavigation({},"Cannot open the selected folder in this window.");
 }catch(const std::exception& error){
  owner_->MessageBox(error.what(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }
 return false;
}

void CHFVExplorerApp::OnNewBrowser(){CreateBrowser();}
void CHFVExplorerApp::OnUpdateNewBrowser(CCmdUI* ui){ui->Enable(!StorageUi::busy()&&!StorageUi::state().sessionOpen);}
CDocument* CHFVExplorerApp::OpenDocumentFile(LPCTSTR path){
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return nullptr;
 if(auto frame=ActiveBrowser()){
  auto doc=frame->GetBrowserSession().Document();
  return doc->OnOpenDocument(path)?doc:nullptr;
 }
 InstallBrowserTemplate();return m_browserTemplate->OpenDocumentFile(path);
}
#ifdef HFV_TESTING
void CHFVExplorerApp::SeedRecentImageForTest(LPCTSTR path)
{
 // The routing test supplies its own MRU and needs no shell or profile access.
 if(!m_pRecentFileList)m_pRecentFileList=new CRecentFileList(0,"Recent File List","File%d",10);
 (*m_pRecentFileList)[0]=path;
}
#endif

void CHFVExplorerApp::OpenRecentImage(UINT command,BrowserSession& session){
 if(StorageUi::busy()||StorageUi::state().sessionOpen||!m_pRecentFileList)return;
 const int index=int(command-ID_FILE_MRU_FILE1);
 if(index<0||index>=m_pRecentFileList->GetSize())return;
 const CString path=(*m_pRecentFileList)[index];
 if(!path.IsEmpty()&&!session.Document()->OnOpenDocument(path) &&
    path==(*m_pRecentFileList)[index])
  m_pRecentFileList->Remove(index);
}
void CHFVExplorerApp::OnExitBrowsers(){
 if(BackgroundCopies::Active()){BackgroundCopies::CancelAll();for(auto frame:m_browsers)frame->SetTimer(9202,100,nullptr);return;}
 if(StorageUi::busy()||StorageUi::state().sessionOpen)return;
 while(!m_browsers.empty()){
  auto frame=m_browsers.back();HWND window=frame->GetSafeHwnd();frame->SendMessage(WM_CLOSE);
  if(IsWindow(window))break; // A launched-document/save prompt cancelled closing.
 }
}
