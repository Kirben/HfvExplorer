// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
class BrowserSession;
namespace BrowserTransfers {
struct Selection {std::vector<FileOperations::Location> items;bool canMove=false;};
Selection Snapshot();
void Publish(Selection,bool append);
void Clear();
bool Any();
bool Paste(BrowserSession&);
enum class Effect {None,Copy,Move};
Effect Choose(const Selection&,const FileOperations::Location&,bool writable,UINT keys);
// Captures a selection separately from the clipboard. Target window/control
// handles are re-resolved at release; no source view or document is retained.
class Drag {
 Selection selection_;
 HWND source_,highlight_=nullptr;
 bool treeHighlight_=false;
 HTREEITEM highlightItem_=nullptr;
 int highlightRow_=-1;
 void ClearHighlight();
public:
 Drag(BrowserSession&);
 ~Drag();
 bool Empty()const{return selection_.items.empty();}
 bool Hover(CPoint screen,UINT keys);
 bool Drop(CPoint screen,UINT keys);
#ifdef HFV_TESTING
 bool TestDrop(BrowserSession&,bool tree,HTREEITEM,int row,UINT keys);
#endif
};
}
