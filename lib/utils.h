#ifndef UTILS_H
#define UTILS_H

#include <arpa/inet.h>
#include <ev.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <chrono>
#include <cstring>

// Third party libs
#include <glog/logging.h>
#include "concurrentqueue.h"
#include <junction/ConcurrentMap_Leapfrog.h>
#include <gflags/gflags.h>

template <typename T1>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T1>;
template <typename T1, typename T2>
using ConcurrentMap = junction::ConcurrentMap_Leapfrog<T1, T2>;

// Get Current Microsecond Timestamp
uint64_t GetMicrosecondTimestamp();

#endif