#include "client_record.h"
namespace dombft{
    void getClientRecordsFromProto(const google::protobuf::RepeatedPtrField<proto::CheckpointClientRecord> &records,
                                   ClientRecords &dst)
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

    void getRecordsDigest(const ClientRecords &records, byte *digest)
    {
        // unsorted data structure will produce non-deterministic digest
        std::map<uint32_t, ClientRecord> sortedRecords;
        for(const auto& [cliId, cliRecord] : records) {
            sortedRecords[cliId] = cliRecord;
        }
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        for(const auto& [cliId, cliRecord] : sortedRecords) {
            SHA256_Update(&ctx, &cliId, sizeof(cliId));
            SHA256_Update(&ctx, &cliRecord.instance_, sizeof(cliRecord.instance_));
            SHA256_Update(&ctx, &cliRecord.lastSeq_, sizeof(cliRecord.lastSeq_));
            std::vector<int> sortedSeqs(cliRecord.missedSeqs_.begin(), cliRecord.missedSeqs_.end());
            for(const auto& s : sortedSeqs)
                SHA256_Update(&ctx, &s, sizeof(s));
        }
        SHA256_Final(digest, &ctx);
        // print records
        for (const auto& record: records) {
            LOG(INFO) << "client id: " << record.first << " instance: " << record.second.instance_ << " lastSeq: " << record.second.lastSeq_;
            for (const auto& seq: record.second.missedSeqs_) {
                LOG(INFO) << "missed seq: " << seq;
            }
        }
    }

    int getRightShiftNumWithRecords(const ClientRecords &records1,const ClientRecords &records2)
    {
        int shiftNum = 0;
        for(const auto& [cliId, cliRecord1] : records1) {
            if (records2.find(cliId) == records2.end()) {
                shiftNum+=cliRecord1.lastSeq_ - cliRecord1.missedSeqs_.size();
                continue;
            }
            const ClientRecord& cliRecord2 = records2.at(cliId);
            shiftNum += static_cast<int>(cliRecord2.missedSeqs_.size() - cliRecord1.missedSeqs_.size());
            if(cliRecord1.lastSeq_ > cliRecord2.lastSeq_) {
                shiftNum += cliRecord1.lastSeq_ - cliRecord2.lastSeq_;
            }
            if(cliRecord1.lastSeq_ < cliRecord2.lastSeq_){
                for (uint32_t i: cliRecord2.missedSeqs_) {
                    if (i > cliRecord1.lastSeq_)
                        shiftNum--;
                }
            }
        }
        return shiftNum;
    }
}



