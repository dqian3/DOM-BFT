#include "processes/process_config.h"

#include "lib/common.h"
#include "lib/fallback_utils.h"
#include "lib/log.h"
#include "lib/message_type.h"
#include "lib/signature_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "lib/checkpoint_collector.h"
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
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    Address receiverAddr_;
    std::vector<Address> clientAddrs_;

    uint32_t f_;
    uint32_t instance_ = 0;

    /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
    SignatureProvider sigProvider_;
    ThreadPool threadpool_;

    std::mutex replicaStateMutex_;

    std::unique_ptr<Endpoint> endpoint_;
    std::unique_ptr<Timer> fallbackStartTimer_;
    std::unique_ptr<Timer> fallbackTimer_;
    std::shared_ptr<Log> log_;

    // State for tracking clients' state
    ClientRecords clientRecords_;
    ClientRecords checkpointClientRecords_;
    // State for commit/checkpoint protocol
    // checkpoint seq -> CheckpointCollector
    std::map<uint32_t, CheckpointCollector> checkpointCollectors_;
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

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender, bool skipVerify = false);
    void handleClientRequest(const dombft::proto::ClientRequest &request);
    void handleCert(const dombft::proto::Cert &cert);
    void handleReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);

    void sendMsgToDst(const google::protobuf::Message &msg, MessageType type, const Address &dst, byte *buf = nullptr);
    void broadcastToReplicas(const google::protobuf::Message &msg, MessageType type, byte *buf = nullptr);
    bool verifyCert(const dombft::proto::Cert &cert);

    // Fallback Helpers
    void startFallback();
    void handleFallbackStart(const dombft::proto::FallbackStart &msg, std::span<byte> sig);

    void replyFromLogEntry(dombft::proto::Reply &reply, uint32_t seq);
    void finishFallback();

    void holdAndSwapCliReq(const proto::ClientRequest &request);

    // dummy fallback PBFT
    inline bool isPrimary() { return instance_ % replicaAddrs_.size() == replicaId_; }
    void doPrePreparePhase();
    void doPreparePhase();
    void doCommitPhase();
    void handlePrePrepare(const dombft::proto::FallbackPrePrepare &msg);
    void handlePrepare(const dombft::proto::FallbackPrepare &msg);
    void handlePBFTCommit(const dombft::proto::FallbackPBFTCommit &msg);

    // helpers
    bool checkAndUpdateClientRecord(const dombft::proto::ClientRequest &clientHeader);
    void reapplyEntriesWithRecord(uint32_t rShiftNum);
    void tryInitCheckpointCollector(uint32_t seq, uint32_t instance, std::optional<ClientRecords> &&records = std::nullopt);

public:
    Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq_ = 0);
    ~Replica();

    void run();
};

}   // namespace dombft