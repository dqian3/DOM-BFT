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
    uint32_t round;

    uint32_t checkpointReplica;
    const dombft::proto::LogCheckpoint *checkpoint;
    std::vector<const dombft::proto::LogEntry *> entries;

    std::string logDigest;
};

struct PBFTState {
    dombft::proto::RepairProposal proposal;
    std::string proposalDigest;
    std::map<uint32_t, dombft::proto::PBFTPrepare> prepares;
    std::map<uint32_t, std::string> prepareSigs;
};

struct ClientRequest {
    uint32_t clientId;
    uint32_t clientSeq;
    std::string requestData;
    std::string padding;

    uint64_t deadline = 0;
};

bool getLogSuffixFromProposal(const dombft::proto::RepairProposal &repairProposal, LogSuffix &logSuffix);

std::vector<ClientRequest> getAbortedEntries(const LogSuffix &logSuffix, std::shared_ptr<Log> log, uint32_t startSeq);

void applySuffix(LogSuffix &logSuffix, std::shared_ptr<Log> log);

#endif