#ifndef CLIENT_RECORD_H
#define CLIENT_RECORD_H

#include "lib/common.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include <glog/logging.h>

#include <map>
#include <set>

// I would put this inside Client Record, but we already have tests that I'm too lazy to rewrite
struct ClientSequence {
    ClientSequence() = default;

    ClientSequence(const dombft::proto::ClientSequence &clientSequence)
    {
        lastSeq_ = clientSequence.last_seq();
        for (const auto &s : clientSequence.missed_seqs()) {
            missedSeqs_.insert(s);
        }
    }

    uint32_t lastSeq_ = 0;
    std::set<uint32_t> missedSeqs_;

    bool contains(uint32_t seq) const;
    bool update(uint32_t newSeq);
    uint32_t size() const;

    int numMissing(const ClientSequence &referenceSequence) const;

    bool operator==(const ClientSequence &other) const;
};

class ClientRecord {
private:
    std::map<uint32_t, ::ClientSequence> sequences;

public:
    ClientRecord() = default;

    ClientRecord(const dombft::proto::ClientRecord &records);

    bool contains(uint32_t clientId, uint32_t seq) const;
    bool update(uint32_t clientId, uint32_t seq);

    std::string digest() const;

    void toProto(dombft::proto::ClientRecord &records) const;
    void toProtoSingleClient(uint32_t clientId, dombft::proto::ClientSequence &clientSequence) const;

    // Computes the number of records in referenceRecord that are misssing in this record
    // Does not check for extra records in this record
    int numMissing(const ClientRecord &referenceRecord) const;

    bool operator==(const ClientRecord &other) const;

    friend std::ostream &operator<<(std::ostream &out, const ClientRecord &record);
};

std::ostream &operator<<(std::ostream &out, const ClientRecord &record);

#endif   // DOM_BFT_CLIENT_RECORD_H