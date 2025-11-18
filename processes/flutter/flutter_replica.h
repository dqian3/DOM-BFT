#pragma once

#include "lib/config/config_manager.h"
#include "lib/config/config_util.h"

#include "lib/common.h"
#include "lib/crypto/hmac_provider.h"
#include "lib/crypto/sig_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
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

struct Candidate {
    uint64_t timestamp;
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

    bool operator<(const Candidate &other) const { return timestamp < other.timestamp; }
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

    // Clock management state
    std::unordered_map<uint32_t, uint64_t> replicaClocks_;   // replica_id -> last known clock time
    uint64_t lockTime_;                                      // 4f + 1 lowest clock among replicas
    uint64_t lastClockBroadcast_;
    static constexpr uint64_t CLOCK_BROADCAST_INTERVAL_MS = 50;   // Broadcast clock every 50ms

    // RBC slow path state
    uint32_t leaderId_;   // Fixed leader (replica 0)

    // Candidate pool: (timestamp, clientId) -> Candidate
    std::map<std::pair<uint64_t, uint32_t>, Candidate> candidatePool_;

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void verifyMessagesThd();
    void processMessagesThd();
    void processClientRequest(const flutter::proto::FlutterClientRequest &request, std::span<byte> sig);
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
    FlutterReplica(uint32_t replicaId);
    ~FlutterReplica();

    void run();
};

}   // namespace dombft