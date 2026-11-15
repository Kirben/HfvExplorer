// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "resource.h"
#include "MfsImage.h"
#include "MfsBrowser.h"
#include <commdlg.h>

class MfsBrowser : public CDialog {
public:
    MfsBrowser(MfsImage& image,CWnd* parent):CDialog(IDD_MFS_BROWSER,parent),image_(image) {}
protected:
    BOOL OnInitDialog() override;
    afx_msg void ExportAppleSingle() { Export(true); }
    afx_msg void ExportData() { Export(false); }
    afx_msg void SelectionChanged(NMHDR*,LRESULT* result);
    DECLARE_MESSAGE_MAP()
private:
    void Export(bool appleSingle);
    MfsImage& image_;
    CListCtrl list_;
};
BEGIN_MESSAGE_MAP(MfsBrowser,CDialog)
    ON_BN_CLICKED(IDC_MFS_EXPORT,ExportAppleSingle)
    ON_BN_CLICKED(IDC_MFS_DATA,ExportData)
    ON_NOTIFY(LVN_ITEMCHANGED,IDC_MFS_LIST,SelectionChanged)
END_MESSAGE_MAP()

static void cell(HWND list,int row,int column,const std::wstring& text) {
    LVITEMW item={};item.iItem=row;item.iSubItem=column;item.pszText=const_cast<wchar_t*>(text.c_str());
    SendMessageW(list,LVM_SETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));
}
BOOL MfsBrowser::OnInitDialog() {
    CDialog::OnInitDialog();list_.SubclassDlgItem(IDC_MFS_LIST,this);
    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
    list_.SendMessage(CCM_SETUNICODEFORMAT,TRUE);
    const char* names[]={"Name","Data bytes","Resource bytes","Type","Creator","Version","Locked"};
    int widths[]={290,90,105,75,75,65,65};
    for(unsigned c=0;c<ARRAYSIZE(names);++c) list_.InsertColumn(c,names[c],c==1||c==2?LVCFMT_RIGHT:LVCFMT_LEFT,widths[c]);
    for(size_t i=0;i<image_.Entries().size();++i) {
        const MfsEntry& e=image_.Entries()[i];std::wstring name=MfsDisplayName(e.name);
        LVITEMW item={};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=const_cast<wchar_t*>(name.c_str());
        SendMessageW(list_.m_hWnd,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
        cell(list_,int(i),1,std::to_wstring(e.forks[0].length));cell(list_,int(i),2,std::to_wstring(e.forks[1].length));
        cell(list_,int(i),3,MfsDisplayName(std::string(reinterpret_cast<const char*>(e.finderInfo),4)));
        cell(list_,int(i),4,MfsDisplayName(std::string(reinterpret_cast<const char*>(e.finderInfo+4),4)));
        cell(list_,int(i),5,std::to_wstring(e.version));cell(list_,int(i),6,e.flags&1?L"Yes":L"No");
    }
    std::wstring title=MfsDisplayName(image_.VolumeName())+L" \u2014 MFS (read-only)";SetWindowTextW(m_hWnd,title.c_str());
    std::wstring status=std::to_wstring(image_.Entries().size())+L" files \u00b7 "+std::to_wstring(image_.FreeBytes())+L" bytes free";
    SetWindowTextW(GetDlgItem(IDC_MFS_STATUS)->m_hWnd,status.c_str());
    if(!image_.Entries().empty()) list_.SetItemState(0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    GetDlgItem(IDC_MFS_EXPORT)->EnableWindow(!image_.Entries().empty());GetDlgItem(IDC_MFS_DATA)->EnableWindow(!image_.Entries().empty());
    return TRUE;
}
void MfsBrowser::SelectionChanged(NMHDR*,LRESULT* result) {
    bool selected=list_.GetNextItem(-1,LVNI_SELECTED)>=0;
    GetDlgItem(IDC_MFS_EXPORT)->EnableWindow(selected);GetDlgItem(IDC_MFS_DATA)->EnableWindow(selected);*result=0;
}
void MfsBrowser::Export(bool appleSingle) {
    int selected=list_.GetNextItem(-1,LVNI_SELECTED);if(selected<0) return;
    std::wstring name=MfsDisplayName(image_.Entries()[selected].name);
    // Only a suggested Windows filename; AppleSingle retains the original name.
    for(wchar_t& c:name) if(c<32 || wcschr(L"<>:\"/\\|?*",c)) c=L'_';
    while(!name.empty() && (name.back()==L'.' || name.back()==L' ')) name.pop_back();
    if(name.empty()) name=L"MFS file";if(name.size()>120) name.resize(120);
    if(image_.Entries()[selected].version) name+=L"_v"+std::to_wstring(image_.Entries()[selected].version);
    if(appleSingle) name+=L".as";
    wchar_t path[32768]={};wcscpy_s(path,name.c_str());
    OPENFILENAMEW dialog={};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=m_hWnd;
    dialog.lpstrFile=path;dialog.nMaxFile=ARRAYSIZE(path);dialog.lpstrTitle=L"Export to a new file";
    dialog.lpstrFilter=appleSingle?L"AppleSingle file (*.as)\0*.as\0All files\0*.*\0\0":L"All files\0*.*\0\0";
    dialog.lpstrDefExt=appleSingle?L"as":NULL;dialog.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&dialog)) return;
    std::string error;
    if(!MfsExportFile(image_,selected,path,appleSingle,error)) AfxMessageBox(error.c_str(),MB_OK|MB_ICONERROR);
    else AfxMessageBox(appleSingle?"Exported both forks, the original name and Finder metadata as AppleSingle.":"Exported the data fork. Use AppleSingle export to preserve both forks and metadata.",MB_OK|MB_ICONINFORMATION);
}
int TryOpenMfsBrowser(const char* path,CWnd* parent) {
    if(!path || !*path) return 0;
    int count=MultiByteToWideChar(CP_ACP,0,path,-1,NULL,0);if(!count) return 0;
    std::wstring wide(count,L'\0');MultiByteToWideChar(CP_ACP,0,path,-1,&wide[0],count);
    MfsImageSource source;std::string error;
    if(!source.Open(wide.c_str(),error) || !MfsImage::Recognizes(source)) return 0;
    MfsImage image;
    if(!image.Open(source)) {AfxMessageBox(("Could not open MFS image: "+image.Error()).c_str(),MB_OK|MB_ICONERROR);return -1;}
    MfsBrowser browser(image,parent);return browser.DoModal()==-1?-1:1;
}
