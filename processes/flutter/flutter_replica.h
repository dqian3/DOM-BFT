#pragma once

#include "lib/config/config_manager.h"
#include "lib/config/config_util.h"

#include "lib/common.h"
#include "lib/crypto/hmac_provider.h"
#include "lib/crypto/sig_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/transport/timer.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"
#include "proto/flutter_proto.pb.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

namespace dombft {

// Tracks committed client sequences, handling out-of-order commits
struct ClientSequenceTracker {
    uint32_t lastCommitted = 0;              // Last contiguous committed sequence
    std::set<uint32_t> outOfOrderCommits;    // Sequences committed beyond lastCommitted

    // Check if a sequence has been committed
    bool isCommitted(uint32_t seq) const {
        if (seq <= lastCommitted) {
            return true;
        }
        return outOfOrderCommits.count(seq) > 0;
    }

    // Mark a sequence as committed
    void commit(uint32_t seq) {
        if (seq <= lastCommitted) {
            return;  // Already committed
        }

        if (seq == lastCommitted + 1) {
            // Directly extends the contiguous range
            lastCommitted = seq;

            // Advance lastCommitted through any contiguous out-of-order commits
            while (outOfOrderCommits.count(lastCommitted + 1) > 0) {
                lastCommitted++;
                outOfOrderCommits.erase(lastCommitted);
            }
        } else {
            // Out of order commit
            outOfOrderCommits.insert(seq);
        }
    }
};

struct Candidate {
    uint64_t bet;
    uint32_t clientId;
    uint32_t clientSeq;
    flutter::proto::FlutterClientRequest request;
    std::string digest;

    // Vote tracking for RBC
    uint32_t acceptVotes = 0;
    uint32_t rejectVotes = 0;
    std::set<uint32_t> votedReplicas;   // Track which replicas have voted

    // Slow path tracking
    bool slowPathInitiated = false;
    bool slowDecision = false;   // Final decision from leader

    // Slow path leader state
    std::map<uint32_t, bool> slowProposals;   // replica_id -> accept/reject

    bool operator<(const Candidate &other) const { return bet < other.bet; }
};

class FlutterReplica {
private:
    // Replica static config
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    std::vector<Address> proxyAddrs_;
    std::vector<Address> clientAddrs_;
    uint32_t numReplicas_;
    uint32_t f_;                 // max Byzantine faults
    uint32_t quorumSize_;        // 3f + 1
    uint32_t superQuorumSize_;   // 4f + 1
    uint32_t useHMAC_;

    uint32_t numVerifyThreads_;

    // Helper classes for signatures and threading
    SignatureProvider sigProvider_;
    HMACProvider hmacProvider_;

    // Control flow/endpoint objects
    BlockingConcurrentQueue<std::vector<byte>> verifyQueue_;
    BlockingConcurrentQueue<std::vector<byte>> processQueue_;
    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::thread processThread_;

    std::unique_ptr<Endpoint> endpoint_;
    std::unique_ptr<Timer> clockTimer_;   // Timer for periodic clock broadcasts

    // Clock management state
    std::unordered_map<uint32_t, uint64_t> replicaClocks_;   // replica_id -> last known clock time
    uint64_t lockTime_;                                      // 4f + 1 lowest clock among replicas
    uint64_t lastClockBroadcast_;
    uint64_t clockBroadcastInterval_;   // Microseconds between clock broadcasts

    // RBC slow path state
    uint32_t leaderId_;   // Fixed leader (replica 0)

    // Candidate pool: (timestamp, clientId) -> Candidate
    std::map<std::pair<uint64_t, uint32_t>, Candidate> candidatePool_;

    // Client state tracking - handles out-of-order commits
    std::unordered_map<uint32_t, ClientSequenceTracker> clientSeqTrackers_;   // client_id -> sequence tracker

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void verifyMessagesThd();
    void processMessagesThd();
    void processClientRequest(const flutter::proto::FlutterClientRequest &request);
    void processFlutterMessage(const flutter::proto::FlutterMessage &msg);

    // Clock management
    void broadcastClock();
    void processFlutterTime(uint32_t senderId, uint64_t clockTime);
    void updateLockTime();

    // RBC proposal handling
    void broadcastRBCProposal(uint32_t clientId, uint64_t bet, bool accept);
    void processRBCProposal(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept);

    // RBC slow path handling
    void sendRBCSlowProposal(uint32_t clientId, uint64_t bet, bool accept);
    void processRBCSlowProposal(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept);
    void sendRBCSlowValue(uint32_t clientId, uint64_t bet, bool accept);
    void processRBCSlowValue(uint32_t senderId, uint32_t clientId, uint64_t bet, bool accept);

    // Observe message handling
    void broadcastObserve(const flutter::proto::FlutterClientRequest &request, uint64_t bet);
    void processObserve(uint32_t senderId, const flutter::proto::FlutterClientRequest &request, uint64_t bet);

    // Candidate management
    void initializeCandidate(const flutter::proto::FlutterClientRequest &request, uint64_t bet);
    void checkCandidatesForCommit();

    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    FlutterReplica(uint32_t replicaId, uint64_t clockBroadcastInterval);
    ~FlutterReplica();

    void run();
};

}   // namespace dombft