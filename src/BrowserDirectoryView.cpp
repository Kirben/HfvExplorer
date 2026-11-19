// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
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
    int image=e.directory?(row.location.IsWindows()?12:0):(row.location.IsWindows()?1:8);
    if(!row.location.IsWindows()&&!e.directory){
        if(!strcmp(type,"APPL"))image=4;
        else if(!strcmp(type,"FFIL")||!strcmp(type,"ffil")||!strcmp(type,"tfil"))image=6;
        else if(!strcmp(type,"DFIL")||!strcmp(type,"dfil"))image=10;
        else if(!strcmp(type,"TEXT")||!strcmp(type,"PICT")||!strcmp(type,"ttxt")||!strcmp(type,"ttro"))image=11;
        else if(!strcmp(type,"ZSYS")||!strcmp(type,"CLIP")||!strcmp(type,"FNDR"))image=7;
        else if(!strcmp(type,"PREF")||!strcmp(type,"pref"))image=9;
        else if(!strcmp(type,"fdrp"))image=0;
    }
    if (row.icon16 || row.icon32) {
        const auto first = m_largeImages.Count();
        const int large = m_largeImages.Add(row.icon32 ? row.icon32 : row.icon16);
        const int small = m_smallImages.Add(row.icon16 ? row.icon16 : row.icon32);
        if (large >= 0 && large == small) image = large;
        else {
            m_largeImages.Trim(first);
            m_smallImages.Trim(first);
        }
    }
    auto value=row;
    value.treeItem=tree;
    const auto data=StoreRow(std::move(value));
    LVITEMW item{};item.mask=LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM;item.iItem=m_item_count;item.pszText=const_cast<wchar_t*>(name.c_str());item.iImage=image;item.lParam=data;
    if(!m_cl->GetItemCount()){item.mask|=LVIF_STATE;item.state=item.stateMask=LVIS_FOCUSED|LVIS_SELECTED;}
    int index=int(SendMessageW(m_cl->m_hWnd,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item)));
    if(index<0){m_browser_rows.erase(data);return index;}
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
    if(GetBrowserSession().finderLayout&&row.location.IsClassic()&&
       (row.initialized||is_inside_magic(row.point.x,row.point.y))&&!(row.point.x==-1&&row.point.y==-1))
    {
        auto& axes = m_iconPositions[data];
        CPoint point(axes.first.Resize(unmagic(row.point.x)-m_mac_origo.x, NativeDpi::Base,m_dpi),
                     axes.second.Resize(unmagic(row.point.y)-m_mac_origo.y, NativeDpi::Base,m_dpi));
        m_cl->SendMessage(LVM_SETITEMPOSITION32,index,LPARAM(&point));
    }
    if(app.m_show_labels&&BrowserDirectory::Label(e))m_cl->SetItemState(index,INDEXTOOVERLAYMASK(BrowserDirectory::Label(e)),LVIS_OVERLAYMASK);
    ++m_item_count;return index;
}
const BrowserDirectory::Row* CHFVExplorerListView::BrowserRowAt(int row)const{
 const auto* binding=BoundRow(GetListCtrl().GetItemData(row));
 return binding&&!binding->parentLink?binding:nullptr;
}
bool CHFVExplorerListView::StorageSelectionAvailable(bool exclusive)const{
 if(!GetBrowserSession().ListingAvailable())return false;
 auto& list=GetListCtrl();
 bool selected=false;
 for(int index=-1;(index=list.GetNextItem(index,LVNI_SELECTED))>=0;){
  const auto row=BrowserRowAt(index);
  if(!row||row->location.IsNone())continue;
  selected=true;
  const auto& location=row->location;
  const auto key=location.IsMounted()?location.Volume()->ResourceKey():
      location.IsWindows()?row->accessKey:location.expectedResource;
  if(!VolumeCoord::Available({{key,exclusive?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read}}))return false;
 }
 if(selected)return true;
 const auto key=GetBrowserSession().MacBrowser().SelectedResourceKey();
 return key.empty()||VolumeCoord::Available({{key,exclusive?VolumeCoord::Mode::Write:VolumeCoord::Mode::Browse}});
}

bool CHFVExplorerListView::BrowserSelectionAllows(bool rename)const{
 if(!GetBrowserSession().ListingAvailable())return false;
 bool selected=false;auto& list=GetListCtrl();
 for(int row=-1;(row=list.GetNextItem(row,LVNI_SELECTED))>=0;){
  const auto model=BrowserRowAt(row);
  if(!model)continue;
  selected=true;
  if(model->location.IsNone()||!model->entry.readError.empty()||!(rename?model->capabilities.rename:model->capabilities.remove))return false;
 }
 return selected;
}
void CHFVExplorerListView::SortBrowserRows(int column,bool ascending){
    CancelBrowserDrag();
    struct Context {const std::map<LPARAM,BrowserDirectory::Row>* rows;int column;bool ascending;} context{&m_browser_rows,column,ascending};
    auto compare=[](LPARAM left,LPARAM right,LPARAM raw)->int {
        auto& c=*reinterpret_cast<Context*>(raw);
        auto x=c.rows->find(left),y=c.rows->find(right);if(x==c.rows->end()||y==c.rows->end())return 0;
        const bool a=x->second.parentLink,b=y->second.parentLink;
        if(a||b)return a==b?0:a?-1:1;
        return BrowserDirectory::Compare(x->second,y->second,c.column,c.ascending);
    };
    m_cl->SortItems(compare,reinterpret_cast<DWORD_PTR>(&context));
}
