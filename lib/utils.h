#ifndef UTILS_H
#define UTILS_H

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

// Third party libs
#include "concurrentqueue.h"
#include "readerwriterqueue.h"
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <junction/ConcurrentMap_Leapfrog.h>

#include "common_struct.h"

template <typename T1> using ConcurrentQueue = moodycamel::ConcurrentQueue<T1>;
template <typename T1> using RWQueue = moodycamel::ReaderWriterQueue<T1>;
template <typename T1> using BlockingRWQueue = moodycamel::BlockingReaderWriterQueue<T1>;
template <typename T1, typename T2> using ConcurrentMap = junction::ConcurrentMap_Leapfrog<T1, T2>;

// Get Current Microsecond Timestamp
int64_t GetMicrosecondTimestamp();

std::string digest_to_hex(const byte *digest, size_t len = SHA256_DIGEST_LENGTH);

std::string digest_to_hex(const std::string &digest);

#endif