
#include "lib/log_checkpoint.h"

LogCheckpoint::LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto)
{
    seq = checkpointProto.seq();
    logDigest = checkpointProto.log_digest();
    appDigest = checkpointProto.app_digest();

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
    : seq(other.seq)
    , logDigest(other.logDigest)
    , appDigest(other.appDigest)
    , commits(other.commits)
    , commitSigs(other.commitSigs)
    , repairCommits(other.repairCommits)
    , repairCommitSigs(other.repairCommitSigs)

{
}

void LogCheckpoint::toProto(dombft::proto::LogCheckpoint &checkpointProto) const
{
    if (seq > 0) {
        checkpointProto.set_seq(seq);
        checkpointProto.set_log_digest(logDigest);
        checkpointProto.set_app_digest(appDigest);

        // We don't actually need to give proofs of the commits in the checkpoint, since
        //  (a) in the fast path the log up to the checkpoint is committed since it is the fast path
        //  (b) in the repair path all n - f - f = (f + 2e + 1) correct replicas will have the previous repiar
        //  checkpoint
        // TODO we should remove these entirely. However, if we have the normal path this is not the case.

        // for (auto x : commits) {
        //     (*checkpointProto.add_commits()) = x.second;
        //     checkpointProto.add_commit_sigs(commitSigs.at(x.first));
        // }

        // for (auto x : repairCommits) {
        //     (*checkpointProto.add_repair_commits()) = x.second;
        //     checkpointProto.add_repair_commit_sigs(repairCommitSigs.at(x.first));
        // }

    } else {
        checkpointProto.set_seq(0);
        checkpointProto.set_app_digest("");
        checkpointProto.set_log_digest("");
    }

    clientRecord_.toProto(*checkpointProto.mutable_client_record());
}