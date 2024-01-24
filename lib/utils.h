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

// Get Current Microsecond Timestamp
uint64_t GetMicrosecondTimestamp();

#endif