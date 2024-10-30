#include "client_record.h"
namespace dombft {
void getClientRecordsFromProto(
    const CheckpointClientRecordsSet &recordsSet, ClientRecords &dst
)
{
    for (const auto &cliRecord : recordsSet.records()) {
        uint32_t cliId = cliRecord.client_id();
        dst[cliId].instance_ = cliRecord.instance();
        dst[cliId].lastSeq_ = cliRecord.last_seq();
        for (const auto &s : cliRecord.missed_seqs())
            dst[cliId].missedSeqs_.insert(s);
    }
}

bool updateRecordWithSeq(ClientRecord &cliRecord, uint32_t newSeq)
{
    if (cliRecord.lastSeq_ < newSeq) {
        for (uint32_t i = cliRecord.lastSeq_ + 1; i < newSeq; i++)
            cliRecord.missedSeqs_.insert(i);
        cliRecord.lastSeq_ = newSeq;
    } else if (cliRecord.missedSeqs_.contains(newSeq)) {
        cliRecord.missedSeqs_.erase(newSeq);
    } else {
        return false;
    }
    return true;
}

void getRecordsDigest(const ClientRecords &records, byte *digest)
{
    // unsorted data structure will produce non-deterministic digest
    std::map<uint32_t, ClientRecord> sortedRecords;
    for (const auto &[cliId, cliRecord] : records) {
        sortedRecords[cliId] = cliRecord;
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (const auto &[cliId, cliRecord] : sortedRecords) {
        SHA256_Update(&ctx, &cliId, sizeof(cliId));
        SHA256_Update(&ctx, &cliRecord.instance_, sizeof(cliRecord.instance_));
        SHA256_Update(&ctx, &cliRecord.lastSeq_, sizeof(cliRecord.lastSeq_));
        std::vector<int> sortedSeqs(cliRecord.missedSeqs_.begin(), cliRecord.missedSeqs_.end());
        for (const auto &s : sortedSeqs)
            SHA256_Update(&ctx, &s, sizeof(s));
    }
    SHA256_Final(digest, &ctx);
    // print records
    for (const auto &record : records) {
        LOG(INFO) << "client id: " << record.first << " instance: " << record.second.instance_
                  << " lastSeq: " << record.second.lastSeq_;
        for (const auto &seq : record.second.missedSeqs_) {
            LOG(INFO) << "missed seq: " << seq;
        }
    }
}

int getRightShiftNumWithRecords(const ClientRecords &records1, const ClientRecords &records2)
{
    int shiftNum = 0;
    for (const auto &[cliId, cliRecord1] : records1) {
        if (records2.find(cliId) == records2.end()) {
            shiftNum += cliRecord1.lastSeq_ - cliRecord1.missedSeqs_.size();
            continue;
        }
        const ClientRecord &cliRecord2 = records2.at(cliId);
        shiftNum += static_cast<int>(cliRecord2.missedSeqs_.size() - cliRecord1.missedSeqs_.size());
        if (cliRecord1.lastSeq_ > cliRecord2.lastSeq_) {
            shiftNum += cliRecord1.lastSeq_ - cliRecord2.lastSeq_;
        }
        if (cliRecord1.lastSeq_ < cliRecord2.lastSeq_) {
            for (uint32_t i : cliRecord2.missedSeqs_) {
                if (i > cliRecord1.lastSeq_)
                    shiftNum--;
            }
        }
    }
    return shiftNum;
}

bool verifyRecordDigestFromProto(const CheckpointClientRecordsSet &recordsSet)
{
    ClientRecords tmpClientRecords;
    getClientRecordsFromProto(recordsSet, tmpClientRecords);
    for (const auto &record : tmpClientRecords) {
        VLOG(6) << "client id: " << record.first << " instance: " << record.second.instance_
                << " lastSeq: " << record.second.lastSeq_;
        for (const auto &seq : record.second.missedSeqs_) {
            VLOG(6) << "missed seq: " << seq;
        }
    }
    byte recordDigest[SHA256_DIGEST_LENGTH];
    getRecordsDigest(tmpClientRecords, recordDigest);
    VLOG(6) << "record digest: " << digest_to_hex(recordDigest, SHA256_DIGEST_LENGTH);
    VLOG(6) << "message digest: " << digest_to_hex(recordsSet.client_records_digest());
    return digest_to_hex(recordDigest, SHA256_DIGEST_LENGTH) == digest_to_hex(recordsSet.client_records_digest());
}

void toProtoClientRecords(CheckpointClientRecordsSet &recordsSet, const std::unordered_map<uint32_t, ClientRecord> &clientRecords)
{
    for (const auto &[cliId, cliRecord] : clientRecords) {
        proto::CheckpointClientRecord *record = recordsSet.add_records();
        record->set_client_id(cliId);
        record->set_instance(cliRecord.instance_);
        record->set_last_seq(cliRecord.lastSeq_);
        for (const uint32_t &missedSeq : cliRecord.missedSeqs_) {
            record->add_missed_seqs(missedSeq);
        }
    }
}


}   // namespace dombft
