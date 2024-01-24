#ifndef UTILS_H
#define UTILS_H

#include <arpa/inet.h>
#include <ev.h>
#include <glog/logging.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include "lib/udp_socket_endpoint.h"

#define CONCAT_UINT32(a, b) ((((uint64_t)a) << 32u) | (uint32_t)b)
/** Get the high/low 32bits of a uint64 */
#define HIGH_32BIT(a) ((uint32_t)(a >> 32))
#define LOW_32BIT(a) ((uint32_t)a)

// Get Current Microsecond Timestamp
uint64_t GetMicrosecondTimestamp();

#endif