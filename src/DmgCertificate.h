// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace Dmg {
class Password;
struct CertificateRecord {std::array<uint8_t,20> id;HfsPlus::Bytes wrapped;};

struct KeyFilePasswordRequired : HfsPlus::Error {
    KeyFilePasswordRequired():HfsPlus::Error("The PFX/P12 key file could not be opened"){}
};

class PrivateKey {
    struct State;
    std::unique_ptr<State> state_;
public:
    explicit PrivateKey(const std::wstring& path,const Password* password=nullptr);
    ~PrivateKey();
    bool Unwrap(const std::vector<CertificateRecord>&,uint8_t* output,uint32_t keyBytes)const;
};
bool FindCertificateKey(const std::vector<CertificateRecord>&,uint8_t* output,uint32_t keyBytes);
#ifdef HFV_TESTING
void UseCertificateStoreForTest(const PrivateKey*);
#endif
}
