#ifndef CERT_COLLECTOR_H
#define CERT_COLELCTOR_H

#include "common.h"
#include "proto/dombft_proto.pb.h"

#include <map>
#include <optional>
#include <vector>
#include <span>

class CertCollector {

public:
    CertCollector(uint32_t f);

    // Inserts reply/signature with move semantics
    // Assumes that reply has already been verified
    size_t insertReply(dombft::proto::Reply &reply, std::span<byte> &sig,
                       const std::shared_ptr<dombft::proto::BatchedReply>& batchedReply);

    bool hasCert();
    const dombft::proto::Cert &getCert();

    uint32_t f_;
    size_t maxMatchSize_;

    // maps from replica
    std::map<uint32_t, std::pair<dombft::proto::Reply, std::shared_ptr<dombft::proto::BatchedReply>>> replies_;
    std::map<uint32_t, std::vector<byte>> signatures_;

    std::optional<dombft::proto::Cert> cert_;
};

#endif

