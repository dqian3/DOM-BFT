#ifndef LOG_CHECKPOINT_H
#define LOG_CHECKPOINT_H

#include "common.h"

#include "lib/client_record.h"
#include "proto/dombft_proto.pb.h"

#include <map>

struct LogCheckpoint {

    // The sequence number of the last committed entry and digest after applying that entry
    uint32_t committedSeq = 0;
    std::string committedLogDigest;

    // State of stable entry, anything before this is truncated
    uint32_t stableSeq = 0;
    std::string stableLogDigest;
    std::string stableAppDigest;

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    ClientRecord clientRecord_;

    LogCheckpoint() = default;
    LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto);
    LogCheckpoint(const LogCheckpoint &other);

    void toProto(dombft::proto::LogCheckpoint &checkpointProto);
};

#endif   // LOG_CHECKPOINT_H