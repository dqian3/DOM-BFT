#include "log.h"

#include "utils.h"

#include <glog/logging.h>

LogEntry::LogEntry()
    : seq(0)
    , client_id(0)
    , client_seq(0)
    , digest("")
{
}

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, const std::string &req, const std::string &prev_digest)
    : seq(s)
    , client_id(c_id)
    , client_seq(c_seq)
    , request(req)
    , result("")
{
    byte digest_bytes[SHA256_DIGEST_LENGTH];

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest.c_str(), prev_digest.length());
    SHA256_Update(&ctx, request.c_str(), request.length());
    SHA256_Final(digest_bytes, &ctx);

    digest = std::string(digest_bytes, digest_bytes + SHA256_DIGEST_LENGTH);
}

void LogEntry::updateDigest(const byte *prev_digest)
{
    byte digest_bytes[SHA256_DIGEST_LENGTH];

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, request.c_str(), request.length());
    SHA256_Final(digest_bytes, &ctx);

    digest = std::string(digest_bytes, digest_bytes + SHA256_DIGEST_LENGTH);
}

LogEntry::~LogEntry() {}

void LogEntry::toProto(dombft::proto::LogEntry &msg) const
{
    msg.set_seq(seq);
    msg.set_client_id(client_id);
    msg.set_client_seq(client_seq);
    msg.set_digest(digest);
    msg.set_request(request);
    msg.set_result(result);
}

std::ostream &operator<<(std::ostream &out, const LogEntry &le)
{
    out << le.seq << ": (" << le.client_id << ", " << le.client_seq << ") " << digest_to_hex(le.digest) << " | ";
    return out;
}
