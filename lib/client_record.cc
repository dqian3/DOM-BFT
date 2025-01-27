#include "client_record.h"
namespace dombft {

bool ClientSequence::contains(uint32_t seq) const { return seq <= lastSeq_ && !missedSeqs_.contains(seq); }

bool ClientSequence::update(uint32_t newSeq)
{
    if (lastSeq_ < newSeq) {
        for (uint32_t i = lastSeq_ + 1; i < newSeq; i++)
            missedSeqs_.insert(i);
        lastSeq_ = newSeq;
    } else if (missedSeqs_.contains(newSeq)) {
        missedSeqs_.erase(newSeq);
    } else {
        return false;
    }
    return true;
}

uint32_t ClientSequence::size() const { return lastSeq_ - missedSeqs_.size(); }

ClientRecord::ClientRecord(const CheckpointClientRecordsSet &recordsSet)
{
    for (const auto &cliRecord : recordsSet.records()) {
        uint32_t id = cliRecord.client_id();
        sequences[id].lastSeq_ = cliRecord.last_seq();

        for (const auto &s : cliRecord.missed_seqs()) {
            sequences[id].missedSeqs_.insert(s);
        }
    }
}

bool ClientRecord::contains(uint32_t clientId, uint32_t seq) const
{
    return sequences.contains(clientId) && sequences.at(clientId).contains(seq);
}

bool ClientRecord::update(uint32_t clientId, uint32_t seq)
{
    // Returns if the sequence has not already been seen
    return sequences[clientId].update(seq);
}

std::string ClientRecord::digest() const
{
    byte digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (const auto &[id, sequence] : sequences) {
        SHA256_Update(&ctx, &id, sizeof(id));
        SHA256_Update(&ctx, &sequence.lastSeq_, sizeof(sequence.lastSeq_));
        for (const auto &s : sequence.missedSeqs_)
            SHA256_Update(&ctx, &s, sizeof(s));
    }
    SHA256_Final(digest, &ctx);
}

void ClientRecord::toProto(CheckpointClientRecordsSet &recordsSet) const
{
    for (const auto &[id, sequence] : sequences) {
        proto::CheckpointClientRecord *record = recordsSet.add_records();
        record->set_client_id(id);
        record->set_last_seq(sequence.lastSeq_);
        for (const uint32_t &s : sequence.missedSeqs_) {
            record->add_missed_seqs(s);
        }
    }
}

int ClientRecord::numMissing(const ClientRecord &referenceRecord) const
{
    int ret = 0;
    for (const auto &[id, otherSequence] : referenceRecord.sequences) {
        // 1. if replica does not have the record for this client, add all req in the checkpoint
        if (!sequences.contains(id)) {
            ret += otherSequence.size();
            continue;
        }

        const ClientSequence &mySequence = sequences.at(id);

        // 2. Get the diff in missed reqs
        //  negatives are fine as then there will be misses in other cliId since the checkpoint interval is a constant
        //
        ret += static_cast<int>(mySequence.missedSeqs_.size()) - static_cast<int>(otherSequence.missedSeqs_.size());

        // 3. include the reqs that replica has not received before this checkpoint
        //   note: case2 deducts the reqs missed in cpRecord that in repRecord.lastSeq_ ~ cpRecord.lastSeq_, so it is
        //  fine to add the diff directly here.
        if (otherSequence.lastSeq_ > mySequence.lastSeq_) {
            ret += otherSequence.lastSeq_ - mySequence.lastSeq_;
        }
        // 4. we add back the missed reqs that has seq > cpRecord.lastSeq_ as they are not missed in this
        //  round of checkpointing but was included in the 2nd case
        if (otherSequence.lastSeq_ < mySequence.lastSeq_) {
            for (uint32_t i : mySequence.missedSeqs_) {
                if (i > otherSequence.lastSeq_)
                    ret--;
            }
        }
    }
    return ret;
}

}   // namespace dombft
