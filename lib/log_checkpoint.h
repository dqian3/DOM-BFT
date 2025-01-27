#ifndef LOG_CHECKPOINT_H
#define LOG_CHECKPOINT_H

#include "common.h"
#include "proto/dombft_proto.pb.h"

struct LogCheckpoint {
    uint32_t seq = 0;
    // TODO shared ptr here so we don't duplicate it from certs.
    std::string logDigest;
    std::string appDigest;
    std::shared_ptr<std::string> appSnapshot;

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    LogCheckpoint();
    LogCheckpoint(const LogCheckpoint &other);

    void LogCheckpoint::toProto(dombft::proto::LogCheckpoint &checkpointProto);
};

#endif   // LOG_CHECKPOINT_H