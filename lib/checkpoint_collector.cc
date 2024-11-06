#include "checkpoint_collector.h"

#include <utility>

namespace dombft {

// TODO(Hao): should be used to resolve the checkpoint overlapping issue later..
void CheckpointCollector::checkpointStart(uint32_t start_seq, uint32_t end_seq, ClientRecords& records) {

    // TODO(Hao): see how often overlapping happens
    if(inProgress){
        LOG(INFO)<< "Checkpointing is IN PROGRESS for seq="<<endSeq_;
    }

    startSeq_ = start_seq;
    endSeq_ = end_seq;
    clientRecords_ = std::move(records);
    replies_.clear();
    replySigs_.clear();
    commits_.clear();
    commitSigs_.clear();
    commitMatchedReplicas_.clear();
    inProgress = true;
    cert_.reset();
}

bool CheckpointCollector::addAndCheckReplyCollection(const Reply &reply, std::span<byte> sig){


    // TODO(Hao): overlapping issue fix here
    replies_[reply.replica_id()] = reply;
    replySigs_[reply.replica_id()] = std::string(sig.begin(), sig.end());
    
    // Don't try finishing the commit if our log hasn't reached the seq being committed
    if (replies_[replicaId_].seq() != endSeq_) {
        VLOG(4) << "Skipping processing of commit messages until we receive our own...";
        return false;
    }

    if (cert_.has_value() && cert_->seq() >= reply.seq()) {
        VLOG(4) << "Checkpoint: already have cert for seq=" << reply.seq() << ", skipping";
        return false;
    }
    std::map<ReplyKeyTuple, std::set<uint32_t>> matchingReplies;

    // Find a cert among a set of replies
    for (const auto &entry : replies_) {
        uint32_t replicaId = entry.first;
        const Reply &reply = entry.second;

        // Already committed
        if (reply.seq() < startSeq_)
            continue;

        VLOG(4) << digest_to_hex(reply.digest()).substr(0, 8) << " " << reply.seq() << " " << reply.instance();

        ReplyKeyTuple key = {reply.digest(), reply.instance(), reply.seq()};

        matchingReplies[key].insert(replicaId);

        bool hasOwnReply = replies_.count(replicaId_) && replies_[replicaId_].seq() == reply.seq();

        // Need 2f + 1 and own reply
        if (matchingReplies[key].size() >= 2 * f_ + 1 && hasOwnReply) {
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
bool CheckpointCollector::addAndCheckCommitCollection(const Commit &commitMsg, std::span<byte> sig) {
    //TODO(Hao) this will drop the earlier commit message when there is an overlapping
    if (startSeq_ > commitMsg.seq()) {
        VLOG(4) << "Checkpoint: already committed for seq=" << commitMsg.seq() << ", skipping";
        return false;
    }
    commits_[commitMsg.replica_id()] = commitMsg;
    commitSigs_[commitMsg.replica_id()] = std::string(sig.begin(), sig.end());

    // verify the record is not tampered by a malicious replica
    if (!verifyRecordDigestFromProto(commitMsg.client_records_set())) {
        VLOG(5) << "Client records from commit msg from replica " << commitMsg.replica_id()
                << " does not match the carried records digest";
        return false;
    }
    std::map<CommitKeyTuple, std::set<uint32_t>> matchingCommits;
    // Find a cert among a set of replies
    for (const auto &[replicaId, commit] : commits_) {
        // Already committed
        if (commit.seq() < startSeq_)
            continue;
        
        CommitKeyTuple key = {
            commit.log_digest(), commit.app_digest(), commit.instance(), commit.seq(),
            commit.client_records_set().client_records_digest()};
        matchingCommits[key].insert(replicaId);

        // TODO see if there's a better way to do this
        bool hasOwnCommit =
            commits_.count(replicaId_) && commits_[replicaId_].seq() == commit.seq();

        // Need 2f + 1 and own commit
        if (matchingCommits[key].size() >= 2 * f_ + 1 && hasOwnCommit) {
            commitMatchedReplicas_ = matchingCommits[key];
            return true;
        }
    }
    return false;
}
} // dombft