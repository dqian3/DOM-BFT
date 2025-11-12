#include "lib/config/config_util.h"
#include "lib/config/config_manager.h"

#include "lib/application.h"
#include "lib/checkpoint_collector.h"
#include "lib/common.h"
#include "lib/crypto/hmac_provider.h"
#include "lib/crypto/sig_provider.h"
#include "lib/log.h"
#include "lib/repair_utils.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <condition_variable>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <thread>

#include <yaml-cpp/yaml.h>

// Request structure for receiver functionality
struct ReceiverRequest {
    dombft::proto::DOMRequest request;
    uint64_t deadline;
    uint32_t clientId;
    bool verified = false;
};

namespace dombft {
class Replica {
private:
    // ========== Replica Configuration ==========
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;

    std::vector<Address> proxyAddrs_;
    std::vector<Address> clientAddrs_;
    uint32_t f_;
    uint32_t quorumSize_;
    uint32_t superQuorumSize_;

    uint32_t checkpointInterval_;
    uint32_t snapshotInterval_;
    uint32_t numVerifyThreads_;

    bool useHMAC_ = false;

    uint64_t repairTimeout_;
    uint64_t repairViewTimeout_;

    // ========== Receiver Configuration ==========
    uint32_t proxyPort_;
    uint32_t numReceivers_;
    Address replicaAddr_;

    bool skipForwarding_;
    bool ignoreDeadlines_;

    // ========== Shared Infrastructure ==========
    SignatureProvider sigProvider_;
    HMACProvider hmacProvider_;

    // Control flow/endpoint objects
    BlockingConcurrentQueue<std::vector<byte>> verifyQueue_;
    BlockingConcurrentQueue<std::vector<byte>> processQueue_;
    BlockingConcurrentQueue<std::pair<uint32_t, AppSnapshot>> snapshotQueue_;

    std::unique_ptr<Endpoint> endpoint_;
    std::unique_ptr<Timer> fwdTimer_;

    // Receiver-specific queues
    std::mutex deadlineQueueMtx_;
    std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<ReceiverRequest>> deadlineQueue_;
    BlockingConcurrentQueue<std::shared_ptr<ReceiverRequest>> receiverVerifyQueue_;

    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::vector<std::thread> receiverVerifyThreads_;
    std::thread processThread_;

    // ========== Replica State ==========
    uint32_t round_ = 1;
    std::shared_ptr<Log> log_;
    std::shared_ptr<Application> app_;

    // State for commit/checkpoint protocol
    CheckpointCollectorStore checkpointCollectors_;
    bool checkpointSnapshotRequested_ = false;

    // State for repair
    bool repair_ = false;
    bool repairSnapshotRequested_ = false;
    bool repairTimeoutSent_ = false;
    uint64_t repairTimeoutStart_ = 0;
    uint64_t repairViewStart_ = 0;

    uint64_t curRoundStartSeq_ = 0;
    std::map<std::pair<uint64_t, uint32_t>, dombft::proto::ClientRequest> repairQueuedReqs_;

    std::map<uint32_t, dombft::proto::RepairTimeout> repairReplicaTimeouts_;
    std::map<uint32_t, std::string> repairReplicaTimeoutSigs_;

    std::optional<dombft::proto::RepairProposal> repairProposal_;
    std::string proposalDigest_;
    std::map<uint32_t, dombft::proto::RepairStart> repairHistorys_;
    std::map<uint32_t, std::string> repairHistorySigs_;
    std::optional<LogSuffix> repairProposalLogSuffix_;

    // State for PBFT
    bool viewChange_ = false;
    uint32_t pbftView_ = 0;
    uint32_t preparedRound_ = UINT32_MAX;
    bool viewPrepared_ = true;
    PBFTState pbftState_;

    std::map<uint32_t, dombft::proto::PBFTPrepare> repairPrepares_;
    std::map<uint32_t, std::string> repairPrepareSigs_;
    std::map<uint32_t, dombft::proto::PBFTCommit> repairPBFTCommits_;
    std::map<uint32_t, std::string> repairCommitSigs_;
    std::map<uint32_t, dombft::proto::PBFTViewChange> pbftViewChanges_;
    std::map<uint32_t, std::string> pbftViewChangeSigs_;

    // State for testing
    bool crashed_;
    uint32_t swapFreq_;
    uint32_t checkpointDropFreq_;
    std::optional<proto::ClientRequest> heldRequest_;

    uint32_t viewChangeFreq_;
    uint32_t viewChangeInst_;
    bool commitLocalInViewChange_ = false;
    uint32_t viewChangeNum_;
    uint32_t viewChangeCounter_ = 0;
    bool holdPrepareOrCommit_ = false;

    // ========== Receiver State ==========
    uint64_t lastCheckTime_ = 0;
    uint64_t lastFwdDeadline_ = 0;
    std::map<uint32_t, uint64_t> lastMeasurementTimes_;
    uint32_t numForwarded_ = 0;
    uint64_t lastStatTime_ = 0;

    // ========== Unified Message Handling ==========
    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    // Replica message handlers
    void verifyMessagesThd();
    void processMessagesThd();
    void checkTimeouts();

    // Receiver message handlers
    void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void receiveBatchedRequests(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void enqueueReceiverRequest(int64_t recv_time, dombft::proto::DOMRequest &request);
    void sendMeasurementReply(const Address &dstAddr, uint64_t owd, uint64_t sendTime);
    void checkDeadlines();
    void forwardRequest(const dombft::proto::DOMRequest &request);
    void receiverVerifyThd(int threadId);

    // ========== Replica Message Processing ==========
    void processClientRequest(const dombft::proto::ClientRequest &request, bool queued = false);
    void processCert(const dombft::proto::Cert &cert);
    void processReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void processCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);
    void processSnapshot(const AppSnapshot &snapshot, uint32_t round);
    void startCheckpoint(bool createSnapshot);
    void processSnapshotRequest(const dombft::proto::SnapshotRequest &snapshotRequest);
    void processSnapshotReply(const dombft::proto::SnapshotReply &snapshotReply);
    void processRepairTimeout(const dombft::proto::RepairTimeout &msg, std::span<byte> sig);
    void processRepairReplyProof(const dombft::proto::RepairReplyProof &msg);
    void processRepairTimeoutProof(const dombft::proto::RepairTimeoutProof &msg);
    void processRepairStart(const dombft::proto::RepairStart &msg, std::span<byte> sig);
    void processPrePrepare(const dombft::proto::PBFTPrePrepare &msg);
    void processPrepare(const dombft::proto::PBFTPrepare &msg, std::span<byte> sig);
    void processPBFTCommit(const dombft::proto::PBFTCommit &msg, std::span<byte> sig);
    void processPBFTViewChange(const dombft::proto::PBFTViewChange &msg, std::span<byte> sig);
    void processPBFTNewView(const dombft::proto::PBFTNewView &msg);
    void processRepairDone(const dombft::proto::RepairDone &msg);

    // Verification methods
    bool verifyCert(const dombft::proto::Cert &cert);
    bool verifyRepairReplyProof(const dombft::proto::RepairReplyProof &proof);
    bool verifyRepairTimeoutProof(const dombft::proto::RepairTimeoutProof &proof);
    bool verifyCheckpoint(const dombft::proto::LogCheckpoint &checkpoint);
    bool verifyRepairLog(const dombft::proto::RepairStart &log);
    bool verifyRepairProposal(const dombft::proto::RepairProposal &proposal);
    bool verifyViewChange(const dombft::proto::PBFTViewChange &viewChange);
    bool verifyRepairDone(const dombft::proto::RepairDone &done);

    // Repair helpers
    void startRepair();
    void finishRepair(const std::vector<::ClientRequest> &abortedReqs);
    void tryFinishRepair();
    void sendRepairSummaryToClients();
    LogSuffix &getRepairLogSuffix();
    void holdAndSwapCliReq(const proto::ClientRequest &request);

    inline bool ifTriggerViewChange() const
    {
        return !viewChange_ && round_ != 0 && round_ == viewChangeInst_ &&
               (viewChangeNum_ == 0 || viewChangeCounter_ < viewChangeNum_);
    }
    inline bool viewChangeByPrepare() const { return ifTriggerViewChange() && !holdPrepareOrCommit_; }
    inline bool viewChangeByCommit() const { return ifTriggerViewChange() && holdPrepareOrCommit_; }

    inline bool isPrimary() { return pbftView_ % replicaAddrs_.size() == replicaId_; }
    uint32_t getPrimary() { return pbftView_ % replicaAddrs_.size(); }
    void startViewChange();
    void doPrePreparePhase(uint32_t round);
    void doPreparePhase();
    void doCommitPhase();
    std::string getProposalDigest(const dombft::proto::RepairProposal &proposal);

    // Sending helpers
    void sendSnapshotRequest(uint32_t replicaId, uint32_t targetSeq);
    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    Replica(
        uint32_t replicaId, bool crashed = false, uint32_t triggerRepairFreq = 0,
        uint32_t viewChangeFreq = 0, bool commitLocalInViewChange = false, uint32_t viewChangeNum = 0,
        uint32_t checkpointDropFreq = 0, bool skipForwarding = false, bool ignoreDeadlines = false
    );
    ~Replica();

    void run();
};

}   // namespace dombft