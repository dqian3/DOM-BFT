
#include "lib/log_checkpoint.h"

LogCheckpoint::LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto)
{
    committedSeq = checkpointProto.committed_seq();
    committedLogDigest = checkpointProto.committed_log_digest();

    stableSeq = checkpointProto.stable_seq();
    stableAppDigest = checkpointProto.stable_app_digest();
    stableLogDigest = checkpointProto.stable_log_digest();

    for (int i = 0; i < checkpointProto.commits_size(); i++) {
        commits[checkpointProto.commits(i).replica_id()] = checkpointProto.commits(i);
        commitSigs[checkpointProto.commits(i).replica_id()] = checkpointProto.commit_sigs(i);
    }

    for (int i = 0; i < checkpointProto.repair_commits_size(); i++) {
        repairCommits[checkpointProto.repair_commits(i).replica_id()] = checkpointProto.repair_commits(i);
        repairCommitSigs[checkpointProto.repair_commits(i).replica_id()] = checkpointProto.repair_commit_sigs(i);
    }

    clientRecord_ = ClientRecord(checkpointProto.client_record());
}

LogCheckpoint::LogCheckpoint(const LogCheckpoint &other)
    : committedSeq(other.committedSeq)
    , committedLogDigest(other.committedLogDigest)
    , stableSeq(other.stableSeq)
    , stableLogDigest(other.stableLogDigest)
    , stableAppDigest(other.stableAppDigest)
    , commits(other.commits)
    , commitSigs(other.commitSigs)
    , repairCommits(other.repairCommits)
    , repairCommitSigs(other.repairCommitSigs)

{
}

void LogCheckpoint::toProto(dombft::proto::LogCheckpoint &checkpointProto) const
{
    if (committedSeq > 0) {
        checkpointProto.set_committed_seq(committedSeq);
        checkpointProto.set_stable_seq(stableSeq);
        checkpointProto.set_stable_app_digest(stableAppDigest);
        checkpointProto.set_stable_log_digest(stableLogDigest);
        checkpointProto.set_committed_log_digest(committedLogDigest);

        for (auto x : commits) {
            (*checkpointProto.add_commits()) = x.second;
            checkpointProto.add_commit_sigs(commitSigs.at(x.first));
        }

        for (auto x : repairCommits) {
            (*checkpointProto.add_repair_commits()) = x.second;
            checkpointProto.add_repair_commit_sigs(repairCommitSigs.at(x.first));
        }

    } else {
        checkpointProto.set_stable_seq(0);
        checkpointProto.set_committed_seq(0);
        checkpointProto.set_stable_app_digest("");
        checkpointProto.set_stable_log_digest("");
        checkpointProto.set_committed_log_digest("");
    }

    clientRecord_.toProto(*checkpointProto.mutable_client_record());
}