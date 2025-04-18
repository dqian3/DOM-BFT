#ifndef LOG_CHECKPOINT_H
#define LOG_CHECKPOINT_H

#include "common.h"

#include "lib/client_record.h"
#include "proto/dombft_proto.pb.h"

#include <map>

struct LogCheckpoint {

    // The sequence number of the last committed entry and digest after applying that entry
    uint32_t seq = 0;
    std::string logDigest;

    std::string appDigest;

    std::map<uint32_t, dombft::proto::Commit> commits;
    std::map<uint32_t, std::string> commitSigs;

    std::map<uint32_t, dombft::proto::PBFTCommit> repairCommits;
    std::map<uint32_t, std::string> repairCommitSigs;

    ClientRecord clientRecord_;

    std::shared_ptr<std::string> snapshot;

    LogCheckpoint() = default;
    LogCheckpoint(const dombft::proto::LogCheckpoint &checkpointProto);
    LogCheckpoint(const LogCheckpoint &other);

    void toProto(dombft::proto::LogCheckpoint &checkpointProto) const;
};

#endif   // LOG_CHECKPOINT_H