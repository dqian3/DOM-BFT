
#include "lib/log_checkpoint.h"

LogCheckpoint::LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto)
{
    seq = checkpointProto.seq();
    appDigest = checkpointProto.app_digest();
    logDigest = checkpointProto.log_digest();

    for (int i = 0; i < checkpointProto.commits_size(); i++) {
        commitMessages[checkpointProto.commits(i).replica_id()] = checkpointProto.commits(i);
        signatures[checkpointProto.commits(i).replica_id()] = checkpointProto.signatures(i);
    }

    clientRecord_ = ClientRecord(checkpointProto.client_record());
}

LogCheckpoint::LogCheckpoint(const LogCheckpoint &other)
    : seq(other.seq)
    , logDigest(other.logDigest)
    , appDigest(other.appDigest)
    , commitMessages(other.commitMessages)
    , signatures(other.signatures)

{
}

void LogCheckpoint::toProto(dombft::proto::LogCheckpoint &checkpointProto)
{
    if (seq > 0) {
        checkpointProto.set_seq(seq);
        checkpointProto.set_app_digest(appDigest);
        checkpointProto.set_log_digest(logDigest);

        for (auto x : commitMessages) {
            (*checkpointProto.add_commits()) = x.second;
            checkpointProto.add_signatures(signatures[x.first]);
        }

    } else {
        checkpointProto.set_seq(0);
        checkpointProto.set_app_digest("");
        checkpointProto.set_log_digest("");
    }

    clientRecord_.toProto(*checkpointProto.mutable_client_record());
}