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

        VLOG(4) << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.round();

        ReplyKeyTuple key = {reply.digest(), reply.round(), reply.seq()};

        matchingReplies[key].insert(replicaId);

        // Need 2f + 1 and own reply
        if (matchingReplies[key].size() >= 2 * f_ + 1) {
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

void ReplyCollector::getCert(dombft::proto::Cert &cert)
{
    assert(cert_.has_value());
    cert = cert_.value();
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

        if (matchingCommits[key].size() >= 2 * f_ + 1) {
            matchedReplicas_ = matchingCommits[key];
            commitToUse_ = commit;
            return true;
        }
    }
    return false;
}

void CommitCollector::getCheckpoint(::LogCheckpoint &checkpoint)
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
        checkpoint.commitMessages[replicaId] = commits_[replicaId];
        checkpoint.signatures[replicaId] = sigs_[replicaId];
    }
}

bool CheckpointCollector::addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
    std::pair<uint32_t, uint32_t> key = {reply.round(), reply.seq()};

    if (!replyCollectors_.contains(key)) {
        replyCollectors_.emplace(key, ReplyCollector(replicaId_, f_, reply.round(), reply.seq()));
    }

    return replyCollectors_.at(key).addAndCheckReply(reply, sig);
}

bool CheckpointCollector::addAndCheckCommit(const dombft::proto::Commit &commit, std::span<byte> sig)
{
    std::pair<uint32_t, uint32_t> key = {commit.round(), commit.committed_seq()};

    if (!commitCollectors_.contains(key)) {
        commitCollectors_.emplace(key, CommitCollector(f_, commit.round(), commit.committed_seq()));
    }

    return commitCollectors_.at(key).addAndCheckCommit(commit, sig);
}

void CheckpointCollector::getCert(uint32_t round, uint32_t seq, dombft::proto::Cert &cert)
{

    std::pair<uint32_t, uint32_t> key = {round, seq};

    assert(replyCollectors_.contains(key));
    replyCollectors_.at(key).getCert(cert);
}

void CheckpointCollector::getCommitToUse(uint32_t round, uint32_t seq, dombft::proto::Commit &commit)
{
    std::pair<uint32_t, uint32_t> key = {round, seq};

    assert(commitCollectors_.contains(key));
    assert(commitCollectors_.at(key).commitToUse_.has_value());
    commit = commitCollectors_.at(key).commitToUse_.value();
}

void CheckpointCollector::getCheckpoint(uint32_t round, uint32_t seq, ::LogCheckpoint &checkpoint)
{
    std::pair<uint32_t, uint32_t> key = {round, seq};

    assert(commitCollectors_.contains(key));
    commitCollectors_.at(key).getCheckpoint(checkpoint);
}

const CheckpointState &CheckpointCollector::getCachedState(uint32_t seq) { return states_.at(seq); }

void CheckpointCollector::cacheState(
    uint32_t seq, const std::string &logDigest, const ::ClientRecord &clientRecord, const AppSnapshot &snapshot
)
{
    if (states_.contains(seq)) {
        VLOG(4) << "Overwriting existing checkpoint state for seq=" << seq;
    }

    // TODO we can look at the current digest here to clean up any stale collectors
    // However, since replica won't process messages from previous rounds, it's also probably fine to not do this...
    states_[seq] = {logDigest, clientRecord, snapshot};
}

void CheckpointCollector::cleanStaleCollectors(uint32_t committedSeq, uint32_t committedRound)
{
    VLOG(1) << "Cleaning up any checkpoint state since checkpoint committed at seq=" << committedSeq
            << " round=" << committedRound;

    std::pair<uint32_t, uint32_t> key = {committedRound, committedSeq};

    states_.erase(states_.begin(), states_.lower_bound(committedSeq));
    replyCollectors_.erase(replyCollectors_.begin(), replyCollectors_.lower_bound(key));
    commitCollectors_.erase(commitCollectors_.begin(), commitCollectors_.lower_bound(key));
}