#ifndef LOG_CHECKPOINT_H
#define LOG_CHECKPOINT_H

#include "common.h"

#include "lib/client_record.h"
#include "proto/dombft_proto.pb.h"

#include <map>

struct LogCheckpoint {

    uint32_t seq = 0;
    // TODO shared ptr here so we don't duplicate it from certs.
    std::string logDigest;

    std::string appSnapshotSeq;
    std::string appDigest;

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    ClientRecord clientRecord_;

    LogCheckpoint() = default;
    LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto);
    LogCheckpoint(const LogCheckpoint &other);

    void toProto(dombft::proto::LogCheckpoint &checkpointProto);
};

#endif   // LOG_CHECKPOINT_H