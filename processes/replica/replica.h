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
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <chrono>

#include <yaml-cpp/yaml.h>


// temp batch resp parameters
#define REPLY_BATCH_SIZE 5 
// if cannot gather 5 resp in 0.1 seconds, send out the batch anyway. 
#define BATCH_TIMEOUT_MS 100

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

    // State for tracking client state
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

    // memebers for the client response batching
    std::mutex batchMutex_;  // Mutex for synchronizing access to the batch
    std::vector<Reply> replyBatch_;  // Buffer to store replies before batching
    std::set<uint32_t> clientsInBatch_;  // Set of client IDs in the current batch
    std::condition_variable batchCondVar_;  // Condition variable for batch processing
    std::chrono::steady_clock::time_point batchStartTime_;  // Time of the last batch processing


    // commented out fro now, maybe we should just use the generic threadpool for signing here
    // use a dedicated batch processing thread to avoid contention with send and recv
    // because batch processing is time sensitive, and contention with others delay the handling
    // std::thread batchProcessingThread_;
    // the shutdown vatiable here is intenderd for clearing the thread when the time ha come .
    // bool shutdown_ = false;



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

    // batch reply
    // void processReplyBatch();

    void processReplyBatch(std::vector<Reply> batchToSend, std::set<uint32_t> clientsToNotify, byte *buffer);
   
    void sendBatchedReplyToClients(const BatchedReply &batchedReply, const std::set<uint32_t> &clients, byte *buffer);


public:
    Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq_ = 0);
    ~Replica();

    void run();
};

}   // namespace dombft