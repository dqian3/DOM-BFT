#include "checkpoint_collector.h"
namespace dombft {

bool CheckpointCollector::addAndCheckReplyCollection(const Reply &reply, std::span<byte> sig){

    replies_[reply.replica_id()] = reply;
    replySigs_[reply.replica_id()] = std::string(sig.begin(), sig.end());
    hasOwnReply_ = hasOwnReply_ || reply.replica_id() == replicaId_;
    // Don't try finishing the commit if our log hasn't reached the seq being committed
    if (!hasOwnReply_) {
        VLOG(4) << "Skipping processing of commit messages until we receive our own...";
        return false;
    }

    if (cert_.has_value()) {
        VLOG(4) << "Checkpoint: already have cert for seq=" << reply.seq() << ", skipping";
        return false;
    }
    std::map<ReplyKeyTuple, std::set<uint32_t>> matchingReplies;

    // Find a cert among a set of replies
    for (const auto &entry : replies_) {
        uint32_t replicaId = entry.first;
        const Reply &reply = entry.second;


        VLOG(4) << digest_to_hex(reply.digest()).substr(0, 8) << " " << reply.seq() << " " << reply.instance();

        ReplyKeyTuple key = {reply.digest(), reply.instance(), reply.seq()};

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
bool CheckpointCollector::addAndCheckCommitCollection(const Commit &commitMsg, const std::span<byte>& sig) {

    // verify the record is not tampered by a malicious replica
    if (!verifyRecordDigestFromProto(commitMsg.client_records_set())) {
        VLOG(5) << "Client records from commit msg from replica " << commitMsg.replica_id()
                << " does not match the carried records digest";
        return false;
    }
    commits_[commitMsg.replica_id()] = commitMsg;
    commitSigs_[commitMsg.replica_id()] = std::string(sig.begin(), sig.end());
    hasOwnCommit_ = hasOwnCommit_ || commitMsg.replica_id() == replicaId_;

    if (!hasOwnCommit_) {
        VLOG(4) << "Skipping processing of commit messages until we receive our own...";
        return false;
    }
    std::map<CommitKeyTuple, std::set<uint32_t>> matchingCommits;
    // Find a cert among a set of replies
    for (const auto &[replicaId, commit] : commits_) {
        
        CommitKeyTuple key = {
            commit.log_digest(), commit.app_digest(), commit.instance(), commit.seq(),
            commit.client_records_set().client_records_digest()};
        matchingCommits[key].insert(replicaId);

        // Need 2f + 1 and own commit
        if (matchingCommits[key].size() >= 2 * f_ + 1) {
            commitMatchedReplicas_ = matchingCommits[key];
            return true;
        }
    }
    return false;
}

CommitResInfo CheckpointCollector::commitToLog(const std::shared_ptr<Log>& log, const dombft::proto::Commit &commit){
    CommitResInfo res = {false, false};
    uint32_t seq = commit.seq();

    LOG(INFO) << "Committing seq=" << seq;
    VLOG(1) << "PERF event=checkpoint_end seq=" << seq;
    res.committed = true;
    log->checkpoint.seq = seq;

    memcpy(log->checkpoint.appDigest, commit.app_digest().c_str(), commit.app_digest().size());
    memcpy(log->checkpoint.logDigest, commit.log_digest().c_str(), commit.log_digest().size());

    for (uint32_t r : commitMatchedReplicas_) {
        log->checkpoint.commitMessages[r] = commits_[r];
        log->checkpoint.signatures[r] = commitSigs_[r];
    }
    log->commit(log->checkpoint.seq);

    const byte *myDigestBytes = log->getDigest(seq);
    std::string myDigest(myDigestBytes, myDigestBytes + SHA256_DIGEST_LENGTH);

    // Modifies log if checkpoint is inconsistent with our current log
    if (myDigest != commit.log_digest()) {
        res.digest_updated = true;
        LOG(INFO) << "Local log digest does not match committed digest, overwriting app snapshot";

        // TODO: counter uses digest as snapshot, need to generalize this
        log->app_->applySnapshot(commit.app_digest());
        VLOG(5) << "Apply commit: old_digest=" << digest_to_hex(myDigest).substr(56)
                << " new_digest=" << digest_to_hex(commit.log_digest()).substr(56);
    }
    return res;
}

} // dombft