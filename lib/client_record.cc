#include "client_record.h"

namespace dombft{
    void getClientRecordsFromProto(const google::protobuf::RepeatedPtrField<proto::CheckpointClientRecord> &records,
                                   std::unordered_map<uint32_t, ClientRecord> &dst)
    {
        for(const auto& cliRecord : records) {
            uint32_t cliId = cliRecord.client_id();
            dst[cliId].instance_ = cliRecord.instance();
            dst[cliId].lastSeq_ = cliRecord.last_seq();
            for(const auto& s : cliRecord.missed_seqs())
                dst[cliId].missedSeqs_.insert(s);
        }
    }

    bool updateRecordWithSeq(ClientRecord& cliRecord, uint32_t newSeq)
    {
        bool updated = true;
        if(cliRecord.lastSeq_ < newSeq) {
            for (uint32_t i = cliRecord.lastSeq_ + 1; i < newSeq; i++)
                cliRecord.missedSeqs_.insert(i);
            cliRecord.lastSeq_ = newSeq;
        } else if(cliRecord.missedSeqs_.contains(newSeq)) {
            cliRecord.missedSeqs_.erase(newSeq);
        } else {
            updated = false;
        }
        return updated;
    }
}
