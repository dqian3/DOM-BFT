#ifndef DOM_BFT_CLIENT_RECORD_H
#define DOM_BFT_CLIENT_RECORD_H

#include "proto/dombft_proto.pb.h"

#include <iostream>

namespace dombft{
    class ClientRecord {
    public:
        uint32_t instance_ = 0;
        uint32_t lastSeq_ = 0;
        std::unordered_set<uint32_t> missedSeqs_;
    };

    template <typename MessageType>
    void toProtoClientRecords(MessageType& message, const std::unordered_map<uint32_t, ClientRecord>& clientRecords)
    {
        for (const auto& [cliId, cliRecord]: clientRecords) {
            proto::CheckpointClientRecord *record = message.add_client_records();
            record->set_client_id(cliId);
            record->set_instance(cliRecord.instance_);
            record->set_last_seq(cliRecord.lastSeq_);
            for (const uint32_t& missedSeq: cliRecord.missedSeqs_) {
                record->add_missed_seqs(missedSeq);
            }
        }
    }

    void getClientRecordsFromProto(const google::protobuf::RepeatedPtrField<proto::CheckpointClientRecord> &records,
                                   std::unordered_map<uint32_t, ClientRecord> &dst);
    bool updateRecordWithSeq(ClientRecord& cliRecord, uint32_t newSeq);
}

#endif //DOM_BFT_CLIENT_RECORD_H