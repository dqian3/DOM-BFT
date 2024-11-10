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

struct LogEntry {
    uint32_t seq;

    uint32_t client_id;
    uint32_t client_seq;

    std::string request;
    std::string result;

    byte digest[SHA256_DIGEST_LENGTH];

    // New member to store verified signatures for the batch
    std::map<uint32_t, std::string> batchSignatures; // Maps replica_id to signature

    LogEntry();

    LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, const std::string &request, const byte *prev_digest);
    ~LogEntry();

    void updateDigest(const byte *prev_digest);

    friend std::ostream &operator<<(std::ostream &out, const LogEntry &le);
};

struct LogCheckpoint {
    uint32_t seq = 0;
    // TODO shared ptr here so we don't duplicate it from certs.
    dombft::proto::Cert cert;
    byte logDigest[SHA256_DIGEST_LENGTH];
    byte appDigest[SHA256_DIGEST_LENGTH];

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    // Default constructor
    LogCheckpoint()
    {
        std::memset(logDigest, 0, SHA256_DIGEST_LENGTH);
        std::memset(appDigest, 0, SHA256_DIGEST_LENGTH);
    }

    // Copy constructor
    LogCheckpoint(const LogCheckpoint &other)
        : seq(other.seq)
        , cert(other.cert)
        , commitMessages(other.commitMessages)
        , signatures(other.signatures)
    {
        std::memcpy(appDigest, other.appDigest, SHA256_DIGEST_LENGTH);
    }
};

struct BatchInfo {
    uint32_t startSeq;
    uint32_t endSeq;

    BatchInfo(uint32_t start, uint32_t end) : startSeq(start), endSeq(end) {}
};

struct Log {

    // Circular buffer of LogEntry, since we know the history won't exceed MAX_SPEC_HIST
    // TODO static memory here? or is that overoptimizing?
    std::array<std::shared_ptr<LogEntry>, MAX_SPEC_HIST> log;

    // Map of sequence number to certs
    std::map<uint32_t, std::shared_ptr<dombft::proto::Cert>> certs;

    LogCheckpoint checkpoint;

    // Map of client ids to sequence numbers, for de-duplicating requests
    std::unordered_map<uint32_t, uint32_t> clientSeqs;

    uint32_t nextSeq;
    uint32_t lastExecuted;

    // The log claims ownership of the application, instead of the replica
    std::shared_ptr<Application> app_;

    Log(std::shared_ptr<Application> app);

    // New map to track batch boundaries
    // std::map<uint32_t, BatchInfo> batchInfoMap; // Key: batch end seq, Value: BatchInfo

    // Adds an entry and returns whether it is successful.
    bool addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res);
    void commit(uint32_t seq);

    void addCert(uint32_t seq, const dombft::proto::Cert &cert);

    const byte *getDigest() const;
    const byte *getDigest(uint32_t seq) const;

    void toProto(dombft::proto::FallbackStart &msg);

    friend std::ostream &operator<<(std::ostream &out, const Log &l);

    std::shared_ptr<LogEntry> getEntry(uint32_t seq);
    void setEntry(uint32_t seq, std::shared_ptr<LogEntry> &entry);
    void rightShiftEntries(uint32_t startSeq, uint32_t num);

    bool inRange(uint32_t seq) const
    {
        return seq < nextSeq && (seq >= nextSeq - MAX_SPEC_HIST || seq < MAX_SPEC_HIST);
    }
    bool canAddEntry() const { return nextSeq <= checkpoint.seq + MAX_SPEC_HIST; }
};

std::ostream &operator<<(std::ostream &out, const LogEntry &le);
std::ostream &operator<<(std::ostream &out, const Log &l);

#endif