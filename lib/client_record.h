#ifndef DOM_BFT_CLIENT_RECORD_H
#define DOM_BFT_CLIENT_RECORD_H

#include "lib/common.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include <glog/logging.h>

namespace dombft {
using namespace dombft::proto;

// I would put this inside Client Record, but we already have tests that I'm too lazy to rewrite
struct ClientSequence {
    uint32_t lastSeq_ = 0;
    std::set<uint32_t> missedSeqs_;

    bool contains(uint32_t seq) const;
    bool update(uint32_t newSeq);
    uint32_t size() const;

    int numMissing(const ClientSequence &referenceSequence) const;
};

class ClientRecord {
private:
    std::map<uint32_t, ClientSequence> sequences;

public:
    ClientRecord() = default;

    ClientRecord(const CheckpointClientRecordSet &records);

    bool contains(uint32_t clientId, uint32_t seq) const;
    bool update(uint32_t clientId, uint32_t seq);

    std::string digest() const;

    void toProto(CheckpointClientRecordSet &records) const;

    // Computes the number of records in referenceRecord that are misssing in this record
    // Does not check for extra records in this record
    int numMissing(const ClientRecord &referenceRecord) const;
};

}   // namespace dombft

#endif   // DOM_BFT_CLIENT_RECORD_H