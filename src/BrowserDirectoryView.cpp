// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HFVExplorer.h"
#include "HFVExplorerListView.h"
#include "BrowserDirectory.h"
#include "BrowserPresentation.h"
#include "special.h"
#include "utils.h"

#ifdef HFV_TESTING
std::function<void(const char*)> BrowserDirectory::testObserveRead;
#endif
int CHFVExplorerListView::InsertBrowserRow(const BrowserDirectory::Row& row,HTREEITEM tree) {
    const auto& e=row.entry;auto name=MacBrowser::Wide(e.name);
    if(e.version)name+=L" [version "+std::to_wstring(e.version)+L"]";
    char type[5]{},creator[5]{};memcpy(type,e.finder.data(),4);memcpy(creator,e.finder.data()+4,4);
    int image=e.directory?(row.storageType==LIST_FAT?12:0):(row.storageType==LIST_FAT?1:8);
    if(row.storageType!=LIST_FAT&&!e.directory){
        if(!strcmp(type,"APPL"))image=4;
        else if(!strcmp(type,"FFIL")||!strcmp(type,"ffil")||!strcmp(type,"tfil"))image=6;
        else if(!strcmp(type,"DFIL")||!strcmp(type,"dfil"))image=10;
        else if(!strcmp(type,"TEXT")||!strcmp(type,"PICT")||!strcmp(type,"ttxt")||!strcmp(type,"ttro"))image=11;
        else if(!strcmp(type,"ZSYS")||!strcmp(type,"CLIP")||!strcmp(type,"FNDR"))image=7;
        else if(!strcmp(type,"PREF")||!strcmp(type,"pref"))image=9;
        else if(!strcmp(type,"fdrp"))image=0;
    }
    if(row.icon16||row.icon32){image=m_pimagelist->GetImageCount();
        m_pimagelist->Add(row.icon32?row.icon32:row.icon16);m_pimagelistSmall->Add(row.icon16?row.icon16:row.icon32);}
    uint64_t created=row.storageType==LIST_FAT?row.created:e.created,modified=row.storageType==LIST_FAT?row.modified:e.modified;
    auto data=make_lparam(row.storageType,row.volume,long(row.id),const_cast<char*>(row.path.c_str()),e.directory,tree,
        row.icon32?row.icon32:row.icon16,row.icon16?row.icon16:row.icon32,DWORD(created),DWORD(created>>32),DWORD(modified),DWORD(modified>>32),BrowserDirectory::Label(e));
    LVITEMW item{};item.mask=LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM;item.iItem=m_item_count;item.pszText=const_cast<wchar_t*>(name.c_str());item.iImage=image;item.lParam=data;
    if(!m_cl->GetItemCount()){item.mask|=LVIF_STATE;item.state=item.stateMask=LVIS_FOCUSED|LVIS_SELECTED;}
    int index=int(SendMessageW(m_cl->m_hWnd,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item)));
    if(index<0)return index;m_browser_rows[data]=row;
    std::wstring size=e.directory?(row.countKnown?std::to_wstring(row.directoryItems)+L" items":L"Folder"):
        std::to_wstring(BrowserDirectory::Bytes(e)/1024+(BrowserDirectory::Bytes(e)%1024!=0))+L" KB";
    if(!e.readError.empty())size=L"Unreadable";
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_SIZE,size);
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_CREATED,BrowserDirectory::DateText(row.created));
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_MODIFIED,BrowserDirectory::DateText(row.modified));
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_TYPE,e.directory?L"Folder":MacBrowser::Wide(BrowserDirectory::FromAnsi(type)));
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_CREATOR,MacBrowser::Wide(BrowserDirectory::FromAnsi(creator)));
    BrowserPresentation::Cell(m_cl->m_hWnd,index,FIELD_LABEL,MacBrowser::Wide(BrowserDirectory::FromAnsi(get_label_text(BrowserDirectory::Label(e)))));
    auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());
    if(app.m_mac_window_mode&&row.storageType==LIST_HFS&&
       (row.initialized||is_inside_magic(row.point.x,row.point.y))&&!(row.point.x==-1&&row.point.y==-1))
        m_cl->SetItemPosition(index,CPoint(unmagic(row.point.x)-m_mac_origo.x,unmagic(row.point.y)-m_mac_origo.y));
    if(app.m_show_labels&&BrowserDirectory::Label(e))m_cl->SetItemState(index,INDEXTOOVERLAYMASK(BrowserDirectory::Label(e)),LVIS_OVERLAYMASK);
    ++m_item_count;return index;
}
const BrowserDirectory::Row* CHFVExplorerListView::BrowserRowAt(int row)const{
 auto key=LPARAM(GetListCtrl().GetItemData(row));auto found=m_browser_rows.find(key);
 return found==m_browser_rows.end()?nullptr:&found->second;
}
bool CHFVExplorerListView::BrowserSelectionAllows(bool rename)const{
 bool selected=false;auto& list=GetListCtrl();
 for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){
  auto binding=reinterpret_cast<list_lparam_struct*>(list.GetItemData(row));if(!binding||binding->type==LIST_SPEC)continue;
  selected=true;auto model=BrowserRowAt(row);if(!model||!model->entry.readError.empty()||!(rename?model->capabilities.rename:model->capabilities.remove))return false;
 }
 return selected;
}
void CHFVExplorerListView::SortBrowserRows(int column,bool ascending){
 // Legacy rename/label operations update the control and binding; keep the
 // presentation copy in sync while retaining exact byte counts and timestamps.
 for(int row=0;row<m_cl->GetItemCount();++row){auto key=LPARAM(m_cl->GetItemData(row));auto found=m_browser_rows.find(key);if(found==m_browser_rows.end())continue;
  wchar_t name[1024]{};LVITEMW value{};value.iSubItem=0;value.pszText=name;value.cchTextMax=ARRAYSIZE(name);
  SendMessageW(m_cl->m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&value));
  if(found->second.storageType!=LIST_MAC)found->second.entry.name=MacBrowser::U16(name);auto p=reinterpret_cast<list_lparam_struct*>(key);
  found->second.entry.finder[9]=BYTE((found->second.entry.finder[9]&~14)|(p->finder_color<<1));
 }

    struct Context {const std::map<LPARAM,BrowserDirectory::Row>* rows;int column;bool ascending;} context{&m_browser_rows,column,ascending};
    auto compare=[](LPARAM left,LPARAM right,LPARAM raw)->int {
        auto& c=*reinterpret_cast<Context*>(raw);auto a=reinterpret_cast<list_lparam_struct*>(left),b=reinterpret_cast<list_lparam_struct*>(right);
        if(!a||!b)return a?1:b?-1:0;
        if(a->type==LIST_SPEC||b->type==LIST_SPEC)return a->type==b->type?0:a->type==LIST_SPEC?-1:1;
        auto x=c.rows->find(left),y=c.rows->find(right);if(x==c.rows->end()||y==c.rows->end())return 0;
        return BrowserDirectory::Compare(x->second,y->second,c.column,c.ascending);
    };
    m_cl->SortItems(compare,reinterpret_cast<DWORD_PTR>(&context));
}
