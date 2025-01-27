#ifndef LOG_H
#define LOG_H

#include "common.h"
#include "proto/dombft_proto.pb.h"

#include <openssl/sha.h>

#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "lib/application.h"
#include "lib/apps/counter.h"

#include "lib/log_checkpoint.h"
#include "lib/log_entry.h"

class Log {

    // Circular buffer of LogEntry, since we know the history won't exceed MAX_SPEC_HIST
    // TODO static memory here? or is that overoptimizing?

private:
    std::deque<LogEntry> log;

    // Map of sequence number to certs
    std::map<uint32_t, std::shared_ptr<dombft::proto::Cert>> certs;

    LogCheckpoint checkpoint;

    // Map of client ids to sequence numbers, for de-duplicating requests
    std::unordered_map<uint32_t, uint32_t> clientSeqs;

    uint32_t nextSeq;
    uint32_t lastExecuted;

    // The log claims ownership of the application, instead of the replica
    std::shared_ptr<Application> app_;

public:
    Log(std::shared_ptr<Application> app);

    bool inRange(uint32_t seq) const;

    // Adds an entry and returns whether it is successful.
    bool addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res);
    bool addCert(uint32_t seq, const dombft::proto::Cert &cert);

    void commit(uint32_t seq);

    const std::string &getDigest() const;
    const std::string &getDigest(uint32_t seq) const;

    void toProto(dombft::proto::FallbackStart &msg);

    const LogEntry &getEntry(uint32_t seq);

    friend std::ostream &operator<<(std::ostream &out, const Log &l);
};

std::ostream &operator<<(std::ostream &out, const Log &l);

#endif