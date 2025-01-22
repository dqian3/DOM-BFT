#ifndef DOM_BFT_CLIENT_RECORD_H
#define DOM_BFT_CLIENT_RECORD_H

#include "lib/common.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include <glog/logging.h>

namespace dombft {
using namespace dombft::proto;
class ClientRecord;
typedef std::unordered_map<uint32_t, ClientRecord> ClientRecords;

class ClientRecord {
public:
    uint32_t lastSeq_ = 0;
    std::unordered_set<uint32_t> missedSeqs_;

    bool updateRecordWithSeq(uint32_t newSeq);
};

void getClientRecordsFromProto(const CheckpointClientRecordsSet &records, ClientRecords &dst);
void getRecordsDigest(const std::unordered_map<uint32_t, ClientRecord> &records, byte *digest);
int getRightShiftNumWithRecords(const ClientRecords &checkpointRecords, const ClientRecords &replicaRecords);

bool verifyRecordDigestFromProto(const CheckpointClientRecordsSet &recordsSet);
void toProtoClientRecords(
    CheckpointClientRecordsSet &recordsSet, const std::unordered_map<uint32_t, ClientRecord> &clientRecords
);
}   // namespace dombft

#endif   // DOM_BFT_CLIENT_RECORD_H