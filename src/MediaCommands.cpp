// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MediaCommands.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "HFVExplorer.h"
#include "MainFrm.h"
#include "MainMacBrowser.h"
#include "MediaControl.h"
#include "MediaDiscovery.h"
#include "ReadOnlyCdBackend.h"

namespace MediaCommands {
namespace {
void Rediscover(DWORD mask)
{
    MediaDiscovery::Changed(mask);
    for(auto frame:static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers){
        auto& session=frame->GetBrowserSession();
        session.NavigationMediaChanged(mask);
        session.MacBrowser().QueueMediaChanges(mask,mask);
    }
}
}

Storage::Result Start(CWnd& owner,int drive,const char* title,const char* operation,const Prepare& prepare)
{
    if(drive<0||drive>=26)return {1,ERROR_BAD_DEVICE,"Invalid removable drive."};
    CommandStorage::Reservation reservation;
    try{
        reservation=std::make_shared<VolumeCoord::Lease>(
            std::vector<VolumeCoord::Access>{{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Write}});
    }catch(const VolumeCoord::Busy&){return CommandStorage::Busy();}

    const auto mask=DWORD(1)<<drive;
    // Construct the worker task before invalidating any browser state.
    bool invalidated=false;
    try{
        auto work=prepare(reservation);
        invalidated=true;
        MediaDiscovery::Changed(mask);
        std::vector<std::shared_ptr<MacBrowser::Volume>> readers;
        for(auto frame:static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers){
            auto& session=frame->GetBrowserSession();
            session.NavigationMediaChanged(mask);
            auto detached=session.MacBrowser().DetachCd(drive);
            readers.insert(readers.end(),std::make_move_iterator(detached.begin()),std::make_move_iterator(detached.end()));
            auto& mounts=session.Mounts();
            for(int index=0;index<mounts.ClassicCount();++index)
                if(MacMediaControlTarget(mounts.ClassicAt(index).m_file_name).drive==drive)
                    mounts.CloseClassic(index);
        }
        BackgroundCopies::StartStorage([drive,readers=std::move(readers),work=std::move(work),reservation=std::move(reservation)](Storage::Job& job){
            for(const auto& reader:readers)reader->ReleaseMedia();
            native_media_suspend(drive);
            if(job.cancelled())throw Storage::Cancelled();
            return work(job);
        },&owner,title,operation,[mask](bool){Rediscover(mask);});
    }catch(const std::exception& error){
        reservation.reset();
        if(invalidated)Rediscover(mask);
        return {1,ERROR_UNHANDLED_EXCEPTION,error.what()};
    }
    return {};
}
}
