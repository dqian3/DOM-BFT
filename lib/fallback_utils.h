#ifndef FALLBACK_UTILS_H
#define FALLBACK_UTILS_H

#include "lib/log.h"
#include "lib/client_record.h"

#include "proto/dombft_proto.pb.h"

// Struct that references the checkpoint and order of entries to use within
// the given FallbackProposal message
struct LogSuffix {
    // For logign purposes
    uint32_t replicaId;
    uint32_t instance;

    const dombft::proto::LogCheckpoint *checkpoint;
    std::vector<const dombft::proto::LogEntry *> entries;
    std::unordered_map<uint32_t , dombft::ClientRecord> clientRecords;
};

bool getLogSuffixFromProposal(const dombft::proto::FallbackProposal &fallbackProposal, LogSuffix &logSuffix);
bool applySuffixToLog(LogSuffix &logSuffix, const std::shared_ptr<Log>& log);

#endif