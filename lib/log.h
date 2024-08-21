#ifndef LOG_H
#define LOG_H

#include "common_struct.h"
#include "proto/dombft_proto.pb.h"
#include "protocol_config.h"

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

    byte *raw_request;
    std::string raw_result;

    uint32_t request_len;

    byte digest[SHA256_DIGEST_LENGTH];

    LogEntry();

    LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, byte *req, uint32_t req_len, byte *prev_digest);
    ~LogEntry();

    friend std::ostream &operator<<(std::ostream &out, const LogEntry &le);
};

struct LogCommitPoint {
    uint32_t seq = 0;
    // TODO shared ptr here so we don't duplicate it from certs.
    std::optional<dombft::proto::Cert> cert;
    byte logDigest[SHA256_DIGEST_LENGTH];
    byte appDigest[SHA256_DIGEST_LENGTH];

    std::map<uint32_t, dombft::proto::Commit> commitMessages;
    std::map<uint32_t, std::string> signatures;

    // Default constructor
    LogCommitPoint() = default;

    // Copy constructor
    LogCommitPoint(const LogCommitPoint &other)
        : seq(other.seq)
        , cert(other.cert)
        , commitMessages(other.commitMessages)
        , signatures(other.signatures)
    {
        std::memcpy(appDigest, other.appDigest, SHA256_DIGEST_LENGTH);
    }
};

struct Log {

    // Circular buffer of LogEntry, since we know the history won't exceed MAX_SPEC_HIST
    // TODO static memory here? or is that overoptimizing?
    std::array<std::shared_ptr<LogEntry>, MAX_SPEC_HIST> log;

    // Map of sequence number to certs
    std::map<uint32_t, std::shared_ptr<dombft::proto::Cert>> certs;

    LogCommitPoint commitPoint;
    // TODO have more than 1 tentative commit point, in case replicas are trying different ones.
    std::optional<LogCommitPoint> tentativeCommitPoint;

    // Map of client ids to sequence numbers, for de-duplicating requests
    std::unordered_map<uint32_t, uint32_t> clientSeqs;

    uint32_t nextSeq;
    uint32_t lastExecuted;

    // The log claims ownership of the application, instead of the replica
    std::unique_ptr<Application> app_;

    Log();

    Log(AppType app_type);

    // Adds an entry and returns whether it is successful.
    bool addEntry(uint32_t c_id, uint32_t c_seq, byte *req, uint32_t req_len);
    bool executeEntry(uint32_t seq);
    bool executeEntry(uint32_t seq, const dombft::proto::ClientRequest &request, dombft::proto::Reply &reply);

    void commit(uint32_t seq);

    // Create a new commit point given the existence of a certificate at seq
    bool createCommitPoint(uint32_t seq);
    // Add a commit message to the commit point,
    bool addCommitMessage(const dombft::proto::Commit &commit, byte *sig, int sigLen);
    // Once 2f + 1 commits are reached the commit point is durable and
    // we can truncate state. The 2f + 1 commits serve as a proof of the commit
    // points validity as well. (TODO can this be f + 1?)
    bool commitCommitPoint();

    void addCert(uint32_t seq, const dombft::proto::Cert &cert);

    const byte *getDigest() const;
    const byte *getDigest(uint32_t seq) const;

    void toProto(dombft::proto::FallbackStart &msg);

    friend std::ostream &operator<<(std::ostream &out, const Log &l);

    LogEntry *getEntry(uint32_t seq);
};

std::ostream &operator<<(std::ostream &out, const LogEntry &le);
std::ostream &operator<<(std::ostream &out, const Log &l);

// Passed around during contention resolution. Will get to this later.
// struct LogSuffix
// {
//     LogCommitPoint base;
//     std::vector<std::unique_ptr<LogEntry>> entries;
//     // TODO shared ptr here so we don't duplicate it from certs.
//     dombft::proto::Cert latestCert;

//     // TODO function to combine 2f + 1 log suffixes into a single one
// };
#endif