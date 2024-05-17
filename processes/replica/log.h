#ifndef LOG_H
#define LOG_H

#include <common_struct.h>

#include <openssl/sha.h>
#include <memory>

class LogEntry {
    uint32_t seq;

    uint32_t client_id;
    uint32_t client_seq;

    std::shared_ptr<byte> client_req;
    byte digest[SHA256_DIGEST_LENGTH];
};


class Log {




};

#endif