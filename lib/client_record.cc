#include "client_record.h"

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

int ClientSequence::numMissing(const ClientSequence &referenceSequence) const
{
    int ret = 0;

    // Remove missed sequneces the reference also missed
    std::set<uint32_t> myMissed;
    std::set_difference(
        missedSeqs_.begin(), missedSeqs_.end(), referenceSequence.missedSeqs_.begin(),
        referenceSequence.missedSeqs_.end(), std::inserter(myMissed, myMissed.begin())
    );

    // First count any numbers we missed, but the reference did not
    auto it = myMissed.upper_bound(referenceSequence.lastSeq_);
    ret += std::distance(myMissed.begin(), it);

    if (lastSeq_ < referenceSequence.lastSeq_) {
        ret += referenceSequence.lastSeq_ - lastSeq_;
    }

    return ret;
}

bool ClientSequence::operator==(const ClientSequence &other) const
{
    return lastSeq_ == other.lastSeq_ && missedSeqs_ == other.missedSeqs_;
}

ClientRecord::ClientRecord(const dombft::proto::ClientRecord &recordProto)
{
    for (const auto &sequence : recordProto.sequences()) {
        uint32_t id = sequence.client_id();
        sequences[id].lastSeq_ = sequence.last_seq();

        for (const auto &s : sequence.missed_seqs()) {
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

    return std::string(digest, digest + SHA256_DIGEST_LENGTH);
}

void ClientRecord::toProto(dombft::proto::ClientRecord &recordProto) const
{
    for (const auto &[id, sequence] : sequences) {
        dombft::proto::ClientSequence *record = recordProto.add_sequences();
        record->set_client_id(id);
        record->set_last_seq(sequence.lastSeq_);
        for (uint32_t s : sequence.missedSeqs_) {
            record->add_missed_seqs(s);
        }
    }

    recordProto.set_digest(digest());
}

void ClientRecord::toProtoSingleClient(uint32_t clientId, dombft::proto::ClientSequence &clientSequence) const
{
    if (!sequences.contains(clientId)) {
        // This could happen if we have not received client requests yet.
        return;
    }

    clientSequence.set_client_id(clientId);
    clientSequence.set_last_seq(sequences.at(clientId).lastSeq_);
    for (uint32_t s : sequences.at(clientId).missedSeqs_) {
        clientSequence.add_missed_seqs(s);
    }
}

// Computes the number of records in referenceRecord that are misssing in this record
// Does not check for extra records in this record
// The purpose of this is to attempt to line up the replica's log with the checkpoints
// Any requests that were covered in the checkpoint will be dropped as duplicates, and
// the next round will see a shift in the log
int ClientRecord::numMissing(const ClientRecord &referenceRecord) const
{
    int ret = 0;
    for (const auto &[id, refSequence] : referenceRecord.sequences) {
        if (!sequences.contains(id)) {
            ret += refSequence.size();
            continue;
        }

        const ClientSequence &mySequence = sequences.at(id);
        ret += mySequence.numMissing(refSequence);
    }
    return ret;
}

bool ClientRecord::operator==(const ClientRecord &other) const { return sequences == other.sequences; }

// Output to stream
std::ostream &operator<<(std::ostream &out, const ClientRecord &record)
{
    out << "ClientRecord: ";
    for (const auto &[id, sequence] : record.sequences) {
        out << "\tClient " << id << " lastSeq=" << sequence.lastSeq_ << " missedSeqs=[";
        for (uint32_t s : sequence.missedSeqs_) {
            out << s << ", ";
        }
        out << "]\n";
    }
    return out;
}