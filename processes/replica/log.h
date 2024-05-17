#ifndef LOG_H
#define LOG_H

#include <common_struct.h>
#include <config.h>

#include <openssl/sha.h>
#include <memory>

#include <unordered_map>
#include <utility>

struct LogEntry
{
    uint32_t seq;

    uint32_t client_id;
    uint32_t client_seq;

    byte *raw_req;

    byte digest[SHA256_DIGEST_LENGTH];

    // Zero initialize everything
    LogEntry()
        : seq(0), client_id(0), client_seq(0), raw_req(nullptr)
    {
        memset(digest, 0, SHA256_DIGEST_LENGTH);
    }

    LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq,
             byte *req, uint32_t req_len, byte *prev_digest)
        : seq(seq)
        , client_id(client_id)
        , client_seq(client_seq)    
        , raw_req((byte * ) malloc(req_len)) // Manually allocate some memory to store the request
    {
        memcpy(raw_req, req, req_len);

        SHA256_CTX ctx;
        SHA256_Init(&ctx);

        SHA256_Update(&ctx, &seq, sizeof(seq));
        SHA256_Update(&ctx, &client_id, sizeof(client_id));
        SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
        SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);
        SHA256_Update(&ctx, &raw_req, req_len);
        SHA256_Final(digest, &ctx);
    }

    ~LogEntry()
     {
        if (raw_req != nullptr) {
            free(raw_req);

        }
    }
};

struct Log
{

    // Circular buffer of LogEntry, since we know the history won't exceed MAX_SPEC_HIST
    // TODO static memory here? or is that overoptimizing?
    std::array<std::unique_ptr<LogEntry>, MAX_SPEC_HIST> log;

    // Map of client ids to sequence numbers, for de-duplicating requests
    std::unordered_map<uint32_t, uint32_t> clientSeqs;

    uint32_t nextSeq;
    uint32_t lastExecuted;
    
    Log();

    void addEntry(uint32_t c_id, uint32_t c_seq,
             byte *req, uint32_t req_len);
    
    bool addAndExecuteEntry(uint32_t c_id, uint32_t c_seq,
             byte *req, uint32_t req_len);
    
    void addCert(uint32_t seq);
};

#endif