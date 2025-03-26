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

std::string digest_to_hex(const std::string &digest)
{
    std::stringstream hexStream;
    hexStream << std::hex << std::setfill('0');

    for (size_t i = 0; i < digest.length(); i++) {
        // We have to cast to byte first to ensure it isn't interpreted as a sign char
        // then to a int to ensure it is printed as a number instead of a char...
        hexStream << std::setw(2) << static_cast<int>(static_cast<byte>(digest[i]));
    }
    std::string ret = hexStream.str();

    // Only print last 8 characters in actual digest, and ensure this is safe in case digest is
    // actually smaller (i.e. for initial checkpoint or testing)
    return ret.substr(std::min(ret.size(), 56ul));
}
