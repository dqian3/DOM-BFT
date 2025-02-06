#ifndef FALLBACK_UTILS_H
#define FALLBACK_UTILS_H

#include "lib/client_record.h"
#include "lib/log.h"

#include "proto/dombft_proto.pb.h"

// Struct that records the checkpoint and order of entries to use within
// the given FallbackProposal
struct LogSuffix {
    // For loging purposes
    uint32_t replicaId;
    uint32_t instance;

    const dombft::proto::LogCheckpoint *checkpoint;
    std::vector<const dombft::proto::LogEntry *> entries;

    ClientRecord clientRecord;
};

struct PBFTState {
    dombft::proto::FallbackProposal proposal;
    byte proposal_digest[SHA256_DIGEST_LENGTH];
    std::map<uint32_t, dombft::proto::PBFTPrepare> prepares;
    std::map<uint32_t, std::string> prepareSigs;
};

bool getLogSuffixFromProposal(const dombft::proto::FallbackProposal &fallbackProposal, LogSuffix &logSuffix);

bool applySuffixWithCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log);

bool applySuffixWithoutCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log);

bool applySuffixAfterCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log);

#endif