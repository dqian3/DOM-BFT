

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

// Compile time configs

// In general, these are for changing major changes to the behavior of the protocols
// rather than for tunable parameters such as timeouts or changeable information such
// as IPs and ports. Generally these will be used for ablation experiments or benchmarks
// of specific components of the system.

#define SEND_BUFFER_SIZE (200000000)
#define UDP_BUFFER_SIZE  (1024)
#define NNG_BUFFER_SIZE  (200000000)
#define IPC_BUFFER_SIZE  (1024)

#define USE_PROXY     1
#define FABRIC_CRYPTO 0
#define SKIP_CRYPTO   0

// For working with dummy protocols
#define SEND_TO_LEADER 0

typedef unsigned char byte;
typedef std::tuple<int, int, int, int, std::string, std::string> ReplyKey;

template <typename T1> using ConcurrentQueue = moodycamel::ConcurrentQueue<T1>;
template <typename T1> using BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T1>;
template <typename T1> using RWQueue = moodycamel::ReaderWriterQueue<T1>;
template <typename T1> using BlockingRWQueue = moodycamel::BlockingReaderWriterQueue<T1>;

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
    CERT = 6,
    CERT_REPLY = 7,
    COMMITTED_REPLY = 8,

    COMMIT = 9,

    REPAIR_CLIENT_TIMEOUT = 10,
    REPAIR_REPLICA_TIMEOUT = 11,
    REPAIR_REPLY_PROOF = 12,
    REPAIR_TIMEOUT_PROOF = 13,

    REPAIR_START = 14,
    REPAIR_PROPOSAL = 15,
    REPAIR_DONE = 16,
    REPAIR_SUMMARY = 17,

    PBFT_PREPREPARE = 18,
    PBFT_PREPARE = 19,
    PBFT_COMMIT = 20,
    PBFT_VIEWCHANGE = 21,
    PBFT_NEWVIEW = 22,

    SNAPSHOT_REQUEST = 23,
    SNAPSHOT_REPLY = 24,

    DUMMY_PROTO = 25
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
    MessageHeader(const uint8_t t, uint32_t l, uint32_t sl)
        : msgType(t)
        , msgLen(l)
        , sigLen(sl) {};
};

// Just to help vscode with recognizing this namespace
namespace dombft {
namespace proto {

}
}   // namespace dombft

#endif