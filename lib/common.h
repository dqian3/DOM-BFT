

#ifndef COMMON_H
#define COMMON_H
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

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

#define SEND_BUFFER_SIZE (200000000)
#define UDP_BUFFER_SIZE  (1024)
#define NNG_BUFFER_SIZE  (200000000)
#define IPC_BUFFER_SIZE  (1024)

#define SKIP_CRYPTO 0

typedef unsigned char byte;
typedef std::tuple<int, int, int, int, std::string, std::string> ReplyKey;

template <typename T1> using ConcurrentQueue = moodycamel::ConcurrentQueue<T1>;
template <typename T1> using BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T1>;
template <typename T1> using RWQueue = moodycamel::ReaderWriterQueue<T1>;
template <typename T1> using BlockingRWQueue = moodycamel::BlockingReaderWriterQueue<T1>;
template <typename T1, typename T2> using ConcurrentMap = junction::ConcurrentMap_Leapfrog<T1, T2>;

enum NodeType { CLIENT, REPLICA };

typedef std::pair<NodeType, uint32_t> NodeID;

/**
 * The message types are defined according to the proto files and the
 * information will be included in each message to facilitate
 * serialize/deserialize proto messages
 */
enum MessageType {
    // DOM Sending Messages
    CLIENT_REQUEST = 1,
    DOM_REQUEST = 2,
    DOM_BATCH_REQUEST = 3,
    MEASUREMENT_REPLY = 4,

    // Fast/normal path messages
    FAST_REPLY = 5,
    REPLY = 6,
    CERT = 7,
    CERT_REPLY = 8,
    COMMITTED_REPLY = 9,

    COMMIT = 10,

    REPAIR_CLIENT_TIMEOUT = 11,
    REPAIR_TIMEOUT = 12,
    REPAIR_REPLY_PROOF = 13,
    REPAIR_TIMEOUT_PROOF = 14,

    REPAIR_START = 15,
    REPAIR_PROPOSAL = 16,
    REPAIR_DONE = 17,
    REPAIR_SUMMARY = 18,

    PBFT_PREPREPARE = 19,
    PBFT_PREPARE = 20,
    PBFT_COMMIT = 21,
    VIEW_UPDATE = 22,

    SNAPSHOT_REQUEST = 24,
    SNAPSHOT_REPLY = 25,

    DUMMY_PROTO = 26,

    MISSING_REQUEST_FETCH = 28,
    MISSING_REQUEST_REPLY = 29,

    // Preserialization messages
    PS_CLIENT = 30,           // Client -> Replica 0
    PS_LEADER_FORWARD = 31,   // Replica 0 -> Others (full mode, contains ClientRequest)
    PS_LEADER_ORDER = 32      // Replica 0 -> Others (order mode, contains seq + digest)
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