#include "processes/process_config.h"

#include "lib/common.h"
#include "lib/fallback_utils.h"
#include "lib/log.h"
#include "lib/signature_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <thread>

#include <yaml-cpp/yaml.h>

namespace dombft {
class Replica {
private:
    // Replica static config
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    Address receiverAddr_;
    std::vector<Address> clientAddrs_;
    uint32_t f_;

    // Helper classes for signatures and threading
    SignatureProvider sigProvider_;

    // Control flow/endpoint objects
    ConcurrentQueue<std::vector<byte>> verifyQueue_;
    ConcurrentQueue<std::vector<byte>> processQueue_;
    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::thread processThread_;

    std::unique_ptr<Endpoint> endpoint_;
    std::unique_ptr<Timer> fallbackStartTimer_;
    std::unique_ptr<Timer> fallbackTimer_;

    // Replica state
    uint32_t instance_ = 0;
    std::shared_ptr<Log> log_;
    ClientRecords clientRecords_;

    // State for commit/checkpoint protocol
    // TODO move this somewhere else?
    // TODO this assumes non-overlapping checkpoint protocol
    uint32_t checkpointSeq_ = 0;
    std::map<int, dombft::proto::Reply> checkpointReplies_;
    std::map<int, std::string> checkpointReplySigs_;
    std::optional<dombft::proto::Cert> checkpointCert_;
    ClientRecords checkpointClientRecords_;
    ClientRecords intermediateCheckpointClientRecords_;

    std::map<uint32_t, dombft::proto::Commit> checkpointCommits_;
    std::map<uint32_t, std::string> checkpointCommitSigs_;

    // State for fallback
    bool fallback_ = false;
    uint32_t fallbackTriggerSeq_ = 0;
    std::optional<dombft::proto::FallbackProposal> fallbackProposal_;
    std::map<int, dombft::proto::FallbackStart> fallbackHistory_;
    std::map<int, std::string> fallbackHistorySigs_;
    std::vector<std::pair<uint64_t, dombft::proto::ClientRequest>> fallbackQueuedReqs_;

    // State for PBFT
    std::map<uint32_t, dombft::proto::FallbackPrepare> fallbackPrepares_;
    std::map<uint32_t, dombft::proto::FallbackPBFTCommit> fallbackPBFTCommits_;

    // State for actively triggering fallback
    uint32_t swapFreq_;
    std::optional<proto::ClientRequest> heldRequest_;

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void verifyMessagesThd();
    void processMessagesThd();

    void processMessage(MessageHeader *msgHdr, byte *msgBuffer);
    void processClientRequest(const dombft::proto::ClientRequest &request);
    void processCert(const dombft::proto::Cert &cert);
    void processReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void processCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);
    void processFallbackTrigger(const dombft::proto::FallbackTrigger &msg);
    void processFallbackStart(const dombft::proto::FallbackStart &msg, std::span<byte> sig);

    bool verifyCert(const dombft::proto::Cert &cert);

    // Fallback Helpers
    void startFallback();
    void replyFromLogEntry(dombft::proto::Reply &reply, uint32_t seq);
    void finishFallback();
    void holdAndSwapCliReq(const proto::ClientRequest &request);

    // dummy fallback PBFT
    inline bool isPrimary() { return instance_ % replicaAddrs_.size() == replicaId_; }
    void doPrePreparePhase();
    void doPreparePhase();
    void doCommitPhase();
    void processPrePrepare(const dombft::proto::FallbackPrePrepare &msg);
    void processPrepare(const dombft::proto::FallbackPrepare &msg);
    void processPBFTCommit(const dombft::proto::FallbackPBFTCommit &msg);

    // helpers for client records
    bool checkAndUpdateClientRecord(const dombft::proto::ClientRequest &clientHeader);
    void reapplyEntriesWithRecord(uint32_t rShiftNum);

    // sending helpers
    // note even though these are templates, we can define them in the cpp file because they are private
    // to this class.
    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq_ = 0);
    ~Replica();

    void run();
};

}   // namespace dombft