#ifndef LOG_H
#define LOG_H

#include "common_struct.h"
#include "protocol_config.h"
#include "proto/dombft_proto.pb.h"

#include <openssl/sha.h>

#include <iostream>
#include <memory>
#include <unordered_map>
#include <utility>

struct LogEntry
{
    uint32_t seq;

    uint32_t client_id;
    uint32_t client_seq;

    byte *raw_request;
    byte *raw_result;

    // if I do not have the length, then having the request does not make sense at all. 
    // because I will not know wehre to stop reading. 
    // and include that in the constructor. 
    uint32_t req_len;
    // maybe we should also have the result length here.
    uint32_t result_len;

    byte digest[SHA256_DIGEST_LENGTH];

    LogEntry();

    LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq,
             byte *req, uint32_t req_len, byte *prev_digest);
    ~LogEntry();

    void logDigest(const byte digest[SHA256_DIGEST_LENGTH]);
    
    friend std::ostream& operator<<(std::ostream &out, const LogEntry &le);
};

struct Log
{

    // Circular buffer of LogEntry, since we know the history won't exceed MAX_SPEC_HIST
    // TODO static memory here? or is that overoptimizing?
    std::array<std::unique_ptr<LogEntry>, MAX_SPEC_HIST> log;

    // Map of sequence number to certs
    std::unordered_map<uint32_t, std::unique_ptr<dombft::proto::Cert>> certs;

    // Map of client ids to sequence numbers, for de-duplicating requests
    std::unordered_map<uint32_t, uint32_t> clientSeqs;

    uint32_t nextSeq;
    uint32_t lastExecuted;

    // as a beginning point for the commit operation, 
    // update each time after the commit operation is done.
    uint32_t lastCommitIdx_;
    
    Log();

    // Adds an entry and returns whether it could be spec. executed.
    bool addEntry(uint32_t c_id, uint32_t c_seq,
             byte *req, uint32_t req_len);

    bool addEntryNormalPath(uint32_t c_id, uint32_t c_seq,
                    byte *req, uint32_t req_len);

    bool executeEntry(uint32_t seq);
    
    void addCert(uint32_t seq, const dombft::proto::Cert &cert);

    const byte* getDigest() const;

    friend std::ostream& operator<<(std::ostream &out, const Log &l);

    void logDigest(const byte digest[SHA256_DIGEST_LENGTH]);
};


std::ostream& operator<<(std::ostream &out, const LogEntry &le);
std::ostream& operator<<(std::ostream &out, const Log &l);
#endif