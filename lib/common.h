

#ifndef COMMON_H
#define COMMON_H
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include <openssl/sha.h>

#include "blockingconcurrentqueue.h"
#include "concurrentqueue.h"
#include "readerwriterqueue.h"
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <junction/ConcurrentMap_Leapfrog.h>

// Compile time configs

// In general, these are for changing major changes to the behavior of the protocols
// rather than for tunable parameters such as timeouts or changeable information such
// as IPs and ports. Generally these will be used for ablation experiments or benchmarks
// of specific components of the system.

#define SEND_BUFFER_SIZE (20000000)
#define UDP_BUFFER_SIZE  (1024)
#define NNG_BUFFER_SIZE  (20000000)
#define IPC_BUFFER_SIZE  (1024)

#define USE_PROXY     1
#define FABRIC_CRYPTO 0
#define SKIP_CRYPTO   0

// For working with dummy protocols
#define SEND_TO_LEADER 0

#define MAX_SPEC_HIST       50000
#define CHECKPOINT_INTERVAL 500

typedef unsigned char byte;
typedef std::tuple<int, int, int, int, std::string, std::string, bool> ReplyKey;

template <typename T1> using ConcurrentQueue = moodycamel::ConcurrentQueue<T1>;
template <typename T1> using BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T1>;
template <typename T1> using RWQueue = moodycamel::ReaderWriterQueue<T1>;
template <typename T1> using BlockingRWQueue = moodycamel::BlockingReaderWriterQueue<T1>;
template <typename T1, typename T2> using ConcurrentMap = junction::ConcurrentMap_Leapfrog<T1, T2>;

/**
 * The message types are defined according to the proto files and the
 * information will be included in each message to facilitate
 * serialize/deserialize proto messages
 */
enum MessageType {
    // DOM Sending Messages
    CLIENT_REQUEST = 1,
    DOM_REQUEST = 2,
    MEASUREMENT_REPLY = 3,

    // Fast/normal path messages
    FAST_REPLY = 4,
    REPLY = 5,
    BATCHED_REPLY = 6,

    CERT = 7,
    CERT_REPLY = 8,

    COMMIT = 9,

    FALLBACK_TRIGGER = 10,
    FALLBACK_START = 11,
    FALLBACK_PROPOSAL = 12,
    FALLBACK_SUMMARY = 13,

    DUMMY_PROTO = 14,

    PBFT_PREPREPARE = 15,
    PBFT_PREPARE = 16,
    PBFT_COMMIT = 17,
    PBFT_VIEWCHANGE = 18,
    PBFT_NEWVIEW = 19,
};

/**
 * When the message has been serialized and is about to be sent by the
 * endpoint, MessageHeader is prepended to the head of message which
 * describes the type of message and its length. In this way, when the
 * receiver endpoint receives the message, it can know the type and length
 * of the proto message, then it can choose the proper way to deserialize it.
 *
 * A signature can be optinally appended to the end of the message as well, with
 * anohter field reporting its len. sigLen = 0 corresponds to no signature
 */
struct MessageHeader {
    uint8_t msgType;
    uint32_t msgLen;
    uint32_t sigLen;
    MessageHeader(const uint8_t t, const uint32_t l, const uint32_t sl)
        : msgType(t)
        , msgLen(l)
        , sigLen(sl){};
};

#endif