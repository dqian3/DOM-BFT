#pragma once
#include "lib/rrr/rrr.hpp"
#include <fstream>
#include <iostream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
// clang-format off

// clang-format on
// Define messages and must write the Marshaling/UnMarshaling method by
// ourselves

struct OOOBenchRequest {
    uint32_t clientId_;
    uint32_t reqId_;
    std::string content_;
};


struct OOOBenchReply {
    uint32_t clientId_;
    uint32_t reqId_;
    std::string content_;
};

namespace rrr {

Marshal &operator<<(Marshal &m, const OOOBenchRequest &msg);

Marshal &operator>>(Marshal &m, OOOBenchRequest &msg);

Marshal &operator<<(Marshal &m, const OOOBenchReply &msg);

Marshal &operator>>(Marshal &m, OOOBenchReply &msg);
}   // namespace rrr
