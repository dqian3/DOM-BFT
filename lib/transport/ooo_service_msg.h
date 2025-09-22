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

struct OOOPrepareRequest {
    uint32_t senderIPInt_;
    uint32_t senderPort_;
    uint32_t length_;
    std::string content_;
};
namespace rrr {

Marshal &operator<<(Marshal &m, const OOOPrepareRequest &msg);

Marshal &operator>>(Marshal &m, OOOPrepareRequest &msg);
}   // namespace rrr
