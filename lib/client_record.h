#ifndef DOM_BFT_CLIENT_RECORD_H
#define DOM_BFT_CLIENT_RECORD_H

#include "lib/common_struct.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include <glog/logging.h>
#include <iostream>

namespace dombft{
    class ClientRecord {
    public:
        uint32_t instance_ = 0;
        uint32_t lastSeq_ = 0;
        std::unordered_set<uint32_t> missedSeqs_;
    };
    typedef std::unordered_map<uint32_t, ClientRecord> ClientRecords;

    void getClientRecordsFromProto(const google::protobuf::RepeatedPtrField<proto::CheckpointClientRecord> &records,
                                   std::unordered_map<uint32_t, ClientRecord> &dst);
    bool updateRecordWithSeq(ClientRecord& cliRecord, uint32_t newSeq);
    void getRecordsDigest(const std::unordered_map<uint32_t , ClientRecord> &records, byte *digest);
    int getRightShiftNumWithRecords(const ClientRecords &records1,const ClientRecords &records2);

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
    template <typename MessageType>
    bool verifyRecordDigestFromProto(const MessageType& message){
        ClientRecords tmpClientRecords;
        getClientRecordsFromProto(message.client_records(), tmpClientRecords);
        for (const auto& record: tmpClientRecords) {
            LOG(INFO) << "client id: " << record.first << " instance: " << record.second.instance_ << " lastSeq: " << record.second.lastSeq_;
            for (const auto& seq: record.second.missedSeqs_) {
                LOG(INFO) << "missed seq: " << seq;
            }
        }
        byte recordDigest[SHA256_DIGEST_LENGTH];
        getRecordsDigest(tmpClientRecords, recordDigest);
        LOG(INFO)<< "record digest: " << digest_to_hex(recordDigest, SHA256_DIGEST_LENGTH);
        LOG(INFO)<< "message digest: " << digest_to_hex(message.client_records_digest());
        return digest_to_hex(recordDigest, SHA256_DIGEST_LENGTH) == digest_to_hex(message.client_records_digest());
    }
}

#endif //DOM_BFT_CLIENT_RECORD_H