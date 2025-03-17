#ifndef REPAIR_UTILS_H
#define REPAIR_UTILS_H

#include "lib/client_record.h"
#include "lib/log.h"

#include "proto/dombft_proto.pb.h"

// Struct that records the checkpoint and order of entries to use within
// the given RepairProposal
struct LogSuffix {
    // For loging purposes
    uint32_t replicaId;
    uint32_t instance;

    uint32_t checkpointReplica;
    const dombft::proto::LogCheckpoint *checkpoint;
    std::vector<const dombft::proto::LogEntry *> entries;
};

struct PBFTState {
    dombft::proto::RepairProposal proposal;
    byte proposal_digest[SHA256_DIGEST_LENGTH];
    std::map<uint32_t, dombft::proto::PBFTPrepare> prepares;
    std::map<uint32_t, std::string> prepareSigs;
};

bool getLogSuffixFromProposal(const dombft::proto::RepairProposal &fallbackProposal, LogSuffix &logSuffix);

void applySuffixWithSnapshot(LogSuffix &logSuffix, std::shared_ptr<Log> log, const std::string &snapshot);

void applySuffixWithoutSnapshot(LogSuffix &logSuffix, std::shared_ptr<Log> log);

void applySuffixAfterCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log);

#endif