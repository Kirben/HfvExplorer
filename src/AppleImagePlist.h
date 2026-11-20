// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlusDisk.h"
#include <atlbase.h>
#include <xmllite.h>
#include <string_view>
#pragma comment(lib,"xmllite.lib")

namespace AppleImagePlist {
inline CComPtr<IXmlReader> Open(HfsPlus::Bytes& xml)
{
    using HfsPlus::Disk::Check;
    // Apple plists commonly name an external DTD. Strip that declaration
    // from the prolog and keep DTD processing disabled.
    constexpr std::string_view prefix="<!DOCTYPE plist";
    const std::string_view text(reinterpret_cast<const char*>(xml.data()),xml.size());
    const auto doctype=text.find(prefix);
    const auto root=text.find("<plist");
    if(doctype!=text.npos&&root!=text.npos&&doctype<root){
        const auto end=text.find('>',doctype);
        Check(end<root&&text.substr(doctype,end-doctype).find('[')==text.npos,
              "Apple disk image plist contains an unsupported DOCTYPE");
        xml.erase(xml.begin()+doctype,xml.begin()+end+1);
    }
    CComPtr<IStream> stream;
    Check(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)),"Cannot allocate Apple disk image plist stream");
    ULONG done=0;
    LARGE_INTEGER zero{};
    Check(SUCCEEDED(stream->Write(xml.data(),ULONG(xml.size()),&done))&&done==xml.size()&&
          SUCCEEDED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)),"Cannot load Apple disk image plist stream");
    CComPtr<IXmlReader> reader;
    Check(SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr))&&
          SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit))&&
          SUCCEEDED(reader->SetInput(stream)),"Cannot initialize Apple disk image plist reader");

    return reader;
}
}
