#ifndef CERT_COLLECTOR_H
#define CERT_COLELCTOR_H

#include "common.h"
#include "proto/dombft_proto.pb.h"

#include <map>
#include <optional>
#include <vector>

class CertCollector {

public:
    CertCollector(int f);

    // Inserts reply/signature with move semantics
    // Assumes that reply has already been verified
    size_t insertReply(dombft::proto::Reply &reply, std::vector<byte> &&sig);

    bool hasCert();
    const dombft::proto::Cert &getCert();

    uint32_t numReceived() const;

    uint32_t f_;

    size_t maxMatchSize_;
    uint32_t instance_;

    // maps from replica to its messages
    std::map<int, dombft::proto::Reply> replies_;
    std::map<int, std::vector<byte>> signatures_;

    std::optional<dombft::proto::Cert> cert_;
};

#endif