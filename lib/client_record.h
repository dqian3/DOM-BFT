#ifndef DOM_BFT_CLIENT_RECORD_H
#define DOM_BFT_CLIENT_RECORD_H

#include "lib/common.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include <glog/logging.h>
#include <iostream>

namespace dombft {
using namespace dombft::proto;

class ClientRecord {
public:
    uint32_t instance_ = 0;
    uint32_t lastSeq_ = 0;
    std::unordered_set<uint32_t> missedSeqs_;
};
typedef std::unordered_map<uint32_t, ClientRecord> ClientRecords;

void getClientRecordsFromProto(
    const CheckpointClientRecordsSet &records,
    std::unordered_map<uint32_t, ClientRecord> &dst
);
bool updateRecordWithSeq(ClientRecord &cliRecord, uint32_t newSeq);
void getRecordsDigest(const std::unordered_map<uint32_t, ClientRecord> &records, byte *digest);
int getRightShiftNumWithRecords(const ClientRecords &records1, const ClientRecords &records2);

bool verifyRecordDigestFromProto(const CheckpointClientRecordsSet &recordsSet);
void toProtoClientRecords(CheckpointClientRecordsSet &recordsSet, const std::unordered_map<uint32_t, ClientRecord> &clientRecords);
}   // namespace dombft

#endif   // DOM_BFT_CLIENT_RECORD_H