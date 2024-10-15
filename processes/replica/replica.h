#include "processes/process_config.h"

#include "lib/log.h"
#include "lib/message_type.h"
#include "lib/protocol_config.h"
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
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    std::vector<Address> clientAddrs_;

    uint32_t f_;
    uint32_t instance_ = 0;

    /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
    SignatureProvider sigProvider_;
    ThreadPool threadpool_;

    std::unique_ptr<Endpoint> endpoint_;
    std::unique_ptr<Timer> fallbackStartTimer_;
    std::unique_ptr<Timer> fallbackTimer_;
    std::shared_ptr<Log> log_;

    // State for tracking client state
    // TODO add some parts for caching client results to deal with duplicate requests
    std::map<int, uint32_t> clientInstance_;

    // State for commit/checkpoint protocol
    // TODO move this somewhere else?
    // TODO this assumes non-overlapping checkpoint protocol
    uint32_t checkpointSeq_ = 0;
    std::map<int, dombft::proto::Reply> checkpointReplies_;
    std::map<int, std::string> checkpointReplySigs_;
    std::optional<dombft::proto::Cert> checkpointCert_;

    std::map<int, dombft::proto::Commit> checkpointCommits_;
    std::map<int, std::string> checkpointCommitSigs_;

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
    void handleClientRequest(const dombft::proto::ClientRequest &request);
    void handleCert(const dombft::proto::Cert &cert);
    void handleReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);

    void broadcastToReplicas(const google::protobuf::Message &msg, MessageType type);
    bool verifyCert(const dombft::proto::Cert &cert);

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

public:
    Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq_ = 0);
    ~Replica();

    void run();
};

}   // namespace dombft