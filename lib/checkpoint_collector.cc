#include "checkpoint_collector.h"

using namespace dombft::proto;

// Collects the reply from peers for the same seq num
// Returns true if it is ok to proceed with the commit stage
bool ReplyCollector::addAndCheckReply(const Reply &reply, std::span<byte> sig)
{
    replies_[reply.replica_id()] = reply;
    replySigs_[reply.replica_id()] = std::string(sig.begin(), sig.end());
    hasOwnReply_ = hasOwnReply_ || reply.replica_id() == replicaId_;
    // Don't try starting commit if our log hasn't reached the seq being committed
    if (!hasOwnReply_) {
        VLOG(4) << "Skipping processing of reply messages until we receive our own...";
        return false;
    }

    if (cert_.has_value()) {
        VLOG(4) << "Checkpoint: already have cert for seq=" << reply.seq() << ", skipping";
        return false;
    }
    std::map<ReplyKeyTuple, std::set<uint32_t>> matchingReplies;

    // Try to generate a cert among a set of replies
    for (const auto &entry : replies_) {
        uint32_t replicaId = entry.first;
        const Reply &reply = entry.second;

        VLOG(4) << replicaId << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.round();

        ReplyKeyTuple key = {reply.digest(), reply.round(), reply.seq()};

        matchingReplies[key].insert(replicaId);

        // Need 2f + 1 and own reply
        if (matchingReplies[key].size() >= 2 * f_ + 1 && matchingReplies[key].contains(replicaId_)) {
            cert_ = Cert();
            cert_->set_seq(std::get<2>(key));

            for (auto repId : matchingReplies[key]) {
                cert_->add_signatures(replySigs_[repId]);
                (*cert_->add_replies()) = replies_[repId];
            }

            VLOG(1) << "Checkpoint: created cert for request number " << reply.seq();
            return true;
        }
    }
    return false;
}

bool CommitCollector::addAndCheckCommit(const Commit &commitMsg, const std::span<byte> sig)
{
    // verify the record is not tampered by a malicious replica
    if (::ClientRecord(commitMsg.client_record()).digest() != commitMsg.client_record().digest()) {
        VLOG(5) << "Client records from commit msg from replica " << commitMsg.replica_id()
                << " does not match the carried records digest";
        return false;
    }
    commits_[commitMsg.replica_id()] = commitMsg;
    sigs_[commitMsg.replica_id()] = std::string(sig.begin(), sig.end());

    std::map<CommitKeyTuple, std::set<uint32_t>> matchingCommits;
    // Find a cert among a set of replies
    for (const auto &[replicaId, commit] : commits_) {

        CommitKeyTuple key = {
            commit.round(),      commit.committed_seq(),     commit.committed_log_digest(),
            commit.stable_seq(), commit.stable_app_digest(), commit.client_record().digest(),
        };
        matchingCommits[key].insert(replicaId);

        VLOG(4) << replicaId << " " << digest_to_hex(commit.committed_log_digest()) << " " << commit.committed_seq();

        if (matchingCommits[key].size() >= 2 * f_ + 1) {
            matchedReplicas_ = matchingCommits[key];
            commitToUse_ = commit;
            return true;
        }
    }
    return false;
}

void CommitCollector::getCheckpoint(::LogCheckpoint &checkpoint) const
{
    // Only valid to get checkpoint if we have colllected enough commits
    assert(commitToUse_.has_value());

    checkpoint.committedSeq = seq_;
    checkpoint.committedLogDigest = commitToUse_->committed_log_digest();

    checkpoint.stableSeq = commitToUse_->stable_seq();
    checkpoint.stableLogDigest = commitToUse_->stable_log_digest();
    checkpoint.stableAppDigest = commitToUse_->stable_app_digest();
    checkpoint.clientRecord_ = ::ClientRecord(commitToUse_->client_record());

    for (uint32_t replicaId : matchedReplicas_) {
        VLOG(6) << "Adding replica commit " << replicaId << " to checkpoint";
        checkpoint.commits[replicaId] = commits_.at(replicaId);
        checkpoint.commitSigs[replicaId] = sigs_.at(replicaId);
    }
}

bool CheckpointCollector::addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
    return replyCollector.addAndCheckReply(reply, sig);
}

void CheckpointCollector::addOwnSnapshot(const AppSnapshot &snapshot) { snapshot_ = snapshot; }

void CheckpointCollector::addOwnState(const std::string &logDigest, const ::ClientRecord &clientRecord)
{
    clientRecord_ = clientRecord;
    logDigest_ = logDigest;
}

bool CheckpointCollector::commitReady() const
{
    return replyCollector.cert_.has_value() && (!needsSnapshot_ || snapshot_.has_value()) &&
           clientRecord_.has_value() && logDigest_.has_value();
}

void CheckpointCollector::getOwnCommit(dombft::proto::Commit &commit) const
{
    commit.set_replica_id(replicaId_);
    commit.set_round(round_);

    commit.set_committed_seq(seq_);
    commit.set_committed_log_digest(logDigest_.value());

    clientRecord_.value().toProto(*commit.mutable_client_record());

    if (needsSnapshot_) {
        commit.set_stable_seq(snapshot_->seq);
        commit.set_stable_log_digest(logDigest_.value());
        commit.set_stable_app_digest(snapshot_->digest);
    } else {
        // TODO, this needs to be filled separately...
    }
}

bool CheckpointCollector::addAndCheckCommit(const dombft::proto::Commit &commit, std::span<byte> sig)
{
    return commitCollector.addAndCheckCommit(commit, sig);
}

void CheckpointCollector::getCheckpoint(::LogCheckpoint &checkpoint) const
{
    commitCollector.getCheckpoint(checkpoint);

    if (needsSnapshot_) {
        checkpoint.snapshot = snapshot_.has_value() ? snapshot_->snapshot : nullptr;
    }
}

// ================= CheckpointCollectorStore =================

bool CheckpointCollectorStore::initCollector(uint32_t round, uint32_t seq, bool needsSnapshot)
{
    std::pair<uint32_t, uint32_t> key = {round, seq};
    std::pair<uint32_t, uint32_t> committedKey = {committedRound_, committedSeq_};

    auto [_, created] = collectors_.try_emplace(key, replicaId_, f_, round, seq, needsSnapshot);
    assert(created);

    return true;
}

bool CheckpointCollectorStore::hasCollector(uint32_t round, uint32_t seq)
{
    std::pair<uint32_t, uint32_t> key = {round, seq};
    return collectors_.contains(key);
}

CheckpointCollector &CheckpointCollectorStore::at(uint32_t round, uint32_t seq)
{
    std::pair<uint32_t, uint32_t> key = {round, seq};
    return collectors_.at(key);
}

void CheckpointCollectorStore::cleanStaleCollectors(uint32_t stableSeq, uint32_t committedSeq)
{
    assert(stableSeq <= committedSeq);

    for (auto it = collectors_.begin(); it != collectors_.end();) {
        const CheckpointCollector &coll = it->second;
        auto [round, seq] = it->first;

        if (coll.needsSnapshot()) {
            if (stableSeq >= seq) {
                it = collectors_.erase(it);
                VLOG(6) << "Cleaning up collector for round=" << round << " seq=" << seq;
            } else {
                ++it;
            }
        } else {
            if (committedSeq >= seq) {
                it = collectors_.erase(it);
                VLOG(6) << "Cleaning up collector for round=" << round << " seq=" << seq;

            } else {
                ++it;
            }
        }
    }
}