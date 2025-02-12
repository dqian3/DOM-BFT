#ifndef LOG_ENTRY_H
#define LOG_ENTRY_H

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

struct LogEntry {
    uint32_t seq;

    uint32_t client_id;
    uint32_t client_seq;

    std::string request;
    std::string result;

    std::string digest;

    LogEntry();

    LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, const std::string &request, const std::string &prev_digest);
    ~LogEntry();

    void updateDigest(const byte *prev_digest);

    // Serialization
    void toProto(dombft::proto::LogEntry &msg) const;
    friend std::ostream &operator<<(std::ostream &out, const LogEntry &le);
};

std::ostream &operator<<(std::ostream &out, const LogEntry &le);

#endif