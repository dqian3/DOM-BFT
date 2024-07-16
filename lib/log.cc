#include "log.h"

#include <glog/logging.h>
#include <sstream>
#include <iomanip>

using namespace dombft::proto;

LogEntry::LogEntry()
    : seq(0), client_id(0), client_seq(0), raw_request(nullptr), raw_result(nullptr)
{
    memset(digest, 0, SHA256_DIGEST_LENGTH);
}

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq,
            byte *req, uint32_t req_len, byte *prev_digest)
    : seq(s)
    , client_id(c_id)
    , client_seq(c_seq)    
    , raw_request((byte * ) malloc(req_len)) // Manually allocate some memory to store the request
    , raw_result(nullptr)
{
    memcpy(raw_request, req, req_len);

    req_len = req_len;

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);

    // Peter: I think this might be a mistake. it should pass in the pointer, not the address of the pointer
    // please check the git log to see what I have changed
    SHA256_Update(&ctx, raw_request, req_len);
    SHA256_Final(digest, &ctx);
}

LogEntry::~LogEntry()
{
    if (raw_request != nullptr) {
        free(raw_request);
    }
    if (raw_result != nullptr) {
        free(raw_result);
    }
}

std::ostream& operator<<(std::ostream &out, const LogEntry& le)
{
    out << le.seq << ": (" << le.client_id << " ," << le.client_seq << ")";
    return out;
}

Log::Log()
    : nextSeq(1), lastExecuted(0)
{
    // Zero initialize all the entries
    // TODO: there's probably a better way to handle this
    for (uint32_t i = 0; i < log.size(); i++) {
        log[i] = std::make_unique<LogEntry>();
    }

}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq,
                   byte *req, uint32_t req_len)
{
    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    byte *prevDigest = log[prevSeq]->digest;

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, req_len, prevDigest);

    logDigest(log[nextSeq % log.size()]->digest);
    
    VLOG(4) << "Adding new entry at seq=" << nextSeq << " c_id=" << c_id
            << " c_seq=" << c_seq;
    nextSeq++;
    // TODO

    return executeEntry(nextSeq - 1);
}

bool Log::addEntryNormalPath(uint32_t c_id, uint32_t c_seq,
                   byte *req, uint32_t req_len)
{
    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    byte *prevDigest = log[prevSeq]->digest;

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, req_len, prevDigest);

    logDigest(log[nextSeq % log.size()]->digest);
    
    VLOG(4) << "Adding new Normal Path entry at seq=" << nextSeq << " c_id=" << c_id
            << " c_seq=" << c_seq;
    nextSeq++;
    // TODO
    return true;
}

bool Log::executeEntry(uint32_t seq)
{
    if (lastExecuted != seq - 1) {
        return false;
    }

    // TODO execute and get result back.
    lastExecuted++;
    return true;
}


void Log::addCert(uint32_t seq, const Cert &cert)
{
    certs[seq] = std::make_unique<Cert>(cert);
}

const byte* Log::getDigest() const
{
    if (nextSeq == 0) {
        return nullptr;
    }
    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    return log[prevSeq]->digest; 
}

std::ostream& operator<<(std::ostream &out, const Log &l)
{
    // go from nextSeq - MAX_SPEC_HIST, which traverses the whole buffer
    // starting from the oldest;
    for (uint32_t i = l.nextSeq - MAX_SPEC_HIST; i < l.nextSeq; i++) {
        int seq = i % MAX_SPEC_HIST;
        out << l.log[seq].get();
    }
    return out;
}


void Log::logDigest(const byte digest[SHA256_DIGEST_LENGTH]) {
    std::stringstream hexStream;
    hexStream << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hexStream << std::setw(2) << static_cast<int>(digest[i]);
    }
    LOG(INFO) << "Digest: " << hexStream.str();
}