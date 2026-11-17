// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <functional>
namespace CdMedia {
struct Track {
 unsigned number=1,session=1;unsigned long long start=0,sectors=0;bool data=true;
 bool operator==(const Track& b)const{return number==b.number&&session==b.session&&start==b.start&&sectors==b.sectors&&data==b.data;}
};
inline DWORD Lba(const BYTE* p){return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3];}
inline bool MsfLba(const BYTE* p,unsigned long long& lba){if(p[1]>=60||p[2]>=75)return false;auto frames=(static_cast<unsigned long long>(p[0])*60+p[1])*75+p[2];if(frames<150)return false;lba=frames-150;return true;}
inline bool Basic(const CDROM_TOC& toc,DWORD returned,std::vector<Track>& tracks,std::string& error){
 tracks.clear();auto fail=[&]{tracks.clear();error="Invalid or truncated CD table of contents";return false;};
 if(returned<FIELD_OFFSET(CDROM_TOC,TrackData)+2*sizeof(TRACK_DATA)||toc.FirstTrack<1||toc.LastTrack>99||toc.LastTrack<toc.FirstTrack)return fail();
 auto count=unsigned(toc.LastTrack-toc.FirstTrack+1);auto needed=FIELD_OFFSET(CDROM_TOC,TrackData)+(count+1)*sizeof(TRACK_DATA);
 unsigned declared=(unsigned(toc.Length[0])<<8)|toc.Length[1];if(returned<needed||(declared&&declared+2!=needed)||toc.TrackData[count].TrackNumber!=0xaa)return fail();
 for(unsigned i=0;i<count;++i){auto& t=toc.TrackData[i];auto start=Lba(t.Address),end=Lba(toc.TrackData[i+1].Address);if(t.TrackNumber!=toc.FirstTrack+i||end<=start)return fail();Track out;out.number=t.TrackNumber;out.start=start;out.sectors=static_cast<unsigned long long>(end)-start;out.data=(t.Control&4)!=0;tracks.push_back(out);}return true;
}
// Full TOC provides each completed session's lead-out. A next-session track
// address alone is not a safe end for the previous session's data track.
inline bool Full(const void* bytes,DWORD returned,std::vector<Track>& tracks,std::string& error){
 tracks.clear();auto fail=[&]{tracks.clear();error="Invalid or incomplete full CD session table";return false;};if(!bytes||returned<4)return fail();auto p=static_cast<const BYTE*>(bytes);unsigned length=(unsigned(p[0])<<8)|p[1];unsigned total=length+2;
 if(total<4||total>returned||(total-4)%sizeof(CDROM_TOC_FULL_TOC_DATA_BLOCK)||p[2]<1||p[3]>99||p[3]<p[2])return fail();
 struct Session{std::map<unsigned,Track> tracks;unsigned first=0,last=0;unsigned long long end=0;};std::map<unsigned,Session> sessions;
 for(unsigned at=4;at<total;at+=sizeof(CDROM_TOC_FULL_TOC_DATA_BLOCK)){
  auto& d=*reinterpret_cast<const CDROM_TOC_FULL_TOC_DATA_BLOCK*>(p+at);if(d.Adr!=1)continue;if(d.SessionNumber<p[2]||d.SessionNumber>p[3])return fail();auto& s=sessions[d.SessionNumber];
  if(d.Point==0xa0){if(s.first||d.Msf[0]<1||d.Msf[0]>99)return fail();s.first=d.Msf[0];}
  else if(d.Point==0xa1){if(s.last||d.Msf[0]<1||d.Msf[0]>99)return fail();s.last=d.Msf[0];}
  else if(d.Point==0xa2){if(s.end||!MsfLba(d.Msf,s.end))return fail();}
  else if(d.Point>=1&&d.Point<=99){Track t;t.number=d.Point;t.session=d.SessionNumber;t.data=(d.Control&4)!=0;if(!MsfLba(d.Msf,t.start)||!s.tracks.emplace(t.number,t).second)return fail();}
 }
 unsigned previousTrack=0;unsigned long long previousEnd=0;
 for(unsigned session=p[2];session<=p[3];++session){auto it=sessions.find(session);if(it==sessions.end())return fail();auto& s=it->second;
  if(!s.first||s.last<s.first||!s.end||s.tracks.size()!=s.last-s.first+1||s.first<=previousTrack)return fail();
  for(unsigned number=s.first;number<=s.last;++number){auto t=s.tracks.find(number);if(t==s.tracks.end())return fail();auto out=t->second;auto next=s.tracks.upper_bound(number);auto end=next==s.tracks.end()?s.end:next->second.start;if(out.start<previousEnd||end<=out.start)return fail();out.sectors=end-out.start;tracks.push_back(out);previousEnd=end;previousTrack=number;}
 }return !tracks.empty();
}

// Query callback keeps native IOCTL orchestration independently testable.
using Query=std::function<bool(CDROM_READ_TOC_EX&,void*,DWORD,DWORD&,DWORD&)>;
inline bool ReadLayout(const Query& query,std::vector<Track>& tracks,DWORD& error,std::string& reason){
 DWORD returned=0;CDROM_READ_TOC_EX request={};request.Format=CDROM_READ_TOC_EX_FORMAT_FULL_TOC;request.Msf=1;request.SessionTrack=1;std::vector<BYTE> full(65536);
 if(query(request,full.data(),DWORD(full.size()),returned,error)){if(!Full(full.data(),returned,tracks,reason)){error=ERROR_BAD_FORMAT;return false;}return true;}
 if(error!=ERROR_NOT_SUPPORTED&&error!=ERROR_INVALID_FUNCTION&&error!=ERROR_INVALID_PARAMETER)return false;
 request={};request.Format=CDROM_READ_TOC_EX_FORMAT_TOC;request.Msf=0;CDROM_TOC toc={};
 if(!query(request,&toc,sizeof(toc),returned,error))return false;if(!Basic(toc,returned,tracks,reason)){error=ERROR_BAD_FORMAT;return false;}
 request={};request.Format=CDROM_READ_TOC_EX_FORMAT_SESSION;request.Msf=0;CDROM_TOC_SESSION_DATA session={};DWORD sessionBytes=0;
 bool known=query(request,&session,sizeof(session),sessionBytes,error)&&sessionBytes>=sizeof(session);
 if(known){
  if(!session.FirstCompleteSession||session.LastCompleteSession<session.FirstCompleteSession||session.LastCompleteSession>99){error=ERROR_BAD_FORMAT;reason="Invalid complete-session information";return false;}
  unsigned first=session.TrackData[0].TrackNumber;auto start=Lba(session.TrackData[0].Address);bool found=false;for(auto& t:tracks)if(t.number==first&&t.start==start)found=true;
  if(!found||(session.FirstCompleteSession==session.LastCompleteSession&&first!=tracks.front().number)){error=ERROR_BAD_FORMAT;reason="The last-session track does not match the CD table";return false;}
  if(session.FirstCompleteSession!=session.LastCompleteSession){
   // Earlier lead-outs are unknown without FULL TOC; never copy session gaps.
   tracks.erase(std::remove_if(tracks.begin(),tracks.end(),[&](const Track& t){return t.number<first;}),tracks.end());}
  for(auto& t:tracks)t.session=session.LastCompleteSession;
 }else if(tracks.size()!=1||tracks[0].start){error=ERROR_NOT_SUPPORTED;reason="The drive cannot report safe CD session boundaries";return false;}
 error=0;return true;
}
inline std::vector<Track> DataTracks(const std::vector<Track>& tracks){std::vector<Track> out;for(auto& t:tracks)if(t.data&&t.sectors)out.push_back(t);return out;}
}
// Retained small helpers now accept any validated single-session data layout.
inline bool CdImageSupportedToc(const CDROM_TOC& toc,DWORD returned){std::vector<CdMedia::Track> tracks;std::string error;return CdMedia::Basic(toc,returned,tracks,error)&&!CdMedia::DataTracks(tracks).empty();}
inline unsigned long long CdImageTocBytes(const CDROM_TOC& toc){std::vector<CdMedia::Track> tracks;std::string error;if(!CdMedia::Basic(toc,sizeof(toc),tracks,error))return 0;auto data=CdMedia::DataTracks(tracks);return data.empty()?0:data.back().sectors*2048;}
