#ifndef CERT_COLLECTOR_H
#define CERT_COLELCTOR_H

#include "common_struct.h"
#include "proto/dombft_proto.pb.h"

#include <map>
#include <optional>
#include <vector>

class CertCollector {

public:
    CertCollector(int f);

    // Inserts reply/signature with move semantics
    // Assumes that reply has already been verified
    bool insertReply(dombft::proto::Reply &reply, std::vector<byte> &sig);

    bool hasCert();
    const dombft::proto::Cert &getCert();

private:
    int f_;
    int maxMatchSize_;

    // maps from replica
    std::map<int, dombft::proto::Reply> replies_;
    std::map<int, std::vector<byte>> signatures_;

    std::optional<dombft::proto::Cert> cert_;

}

// Util function that I just put here for some reson
bool verifyCert(const dombft::proto::Cert &cert);

#endif