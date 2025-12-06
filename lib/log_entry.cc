#include "log.h"

#include "utils.h"

#include <glog/logging.h>

#include <cryptopp/filters.h>
#include <cryptopp/secblock.h>
#include <cryptopp/sha.h>

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
    CryptoPP::SHA256 hash;
    std::string input;

    // Append binary representations
    input.append(reinterpret_cast<const char *>(&seq), sizeof(seq));
    input.append(reinterpret_cast<const char *>(&client_id), sizeof(client_id));
    input.append(reinterpret_cast<const char *>(&client_seq), sizeof(client_seq));
    input.append(prev_digest);
    input.append(request);

    // Note, StringSource takes ownership of these objects
    CryptoPP::StringSource ss(input, true, new CryptoPP::HashFilter(hash, new CryptoPP::StringSink(digest)));
}

LogEntry::~LogEntry() {}

void LogEntry::toProto(dombft::proto::LogEntry &msg, bool includeFullRequest) const
{
    msg.set_seq(seq);
    msg.set_client_id(client_id);
    msg.set_client_seq(client_seq);
    msg.set_digest(digest);

    // Always include the request digest for matching
    CryptoPP::SHA256 hash;
    std::string request_digest;
    CryptoPP::StringSource ss(request, true, new CryptoPP::HashFilter(hash, new CryptoPP::StringSink(request_digest)));
    msg.set_request_digest(request_digest);

    // Optionally include the full request data to avoid fetching
    if (includeFullRequest) {
        msg.set_request(request);
    } else {
        msg.clear_request();
    }
}

std::ostream &operator<<(std::ostream &out, const LogEntry &le)
{
    out << le.seq << ": (" << le.client_id << ", " << le.client_seq << ") size=" << le.request.size() << " "
        << digest_to_hex(le.digest) << " | ";
    return out;
}
