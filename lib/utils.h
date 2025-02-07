#ifndef UTILS_H
#define UTILS_H

#include <openssl/sha.h>
#include <string>

// Third party libs
#include "common.h"

// Get Current Microsecond Timestamp
int64_t GetMicrosecondTimestamp();

std::string digest_to_hex(const std::string &digest);

#endif