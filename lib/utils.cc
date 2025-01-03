#include "lib/utils.h"

#include <chrono>
#include <iomanip>
#include <sstream>

// Get Current Microsecond Timestamp
int64_t GetMicrosecondTimestamp()
{
    auto tse = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(tse).count();
}

std::string digest_to_hex(const byte *digest, size_t len)
{
    std::stringstream hexStream;
    hexStream << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        hexStream << std::setw(2) << static_cast<int>(digest[i]);
    }
    return hexStream.str();
}

std::string digest_to_hex(const std::string &digest)
{
    // assert(digest.size() == SHA256_DIGEST_LENGTH);
    return digest_to_hex((const byte *) digest.c_str(), digest.size());
}
