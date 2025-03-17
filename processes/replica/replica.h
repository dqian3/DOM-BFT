#include "processes/process_config.h"

#include "lib/common.h"

#include "lib/app_snapshot_store.h"
#include "lib/application.h"
#include "lib/checkpoint_collector.h"
#include "lib/log.h"
#include "lib/repair_utils.h"
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
    uint32_t checkpointInterval_;
    uint32_t numVerifyThreads_;
    uint64_t fallbackTimeout_;
    uint64_t viewChangeTimeout_;

    // Helper classes for signatures and threading
    SignatureProvider sigProvider_;

    // Control flow/endpoint objects
    BlockingConcurrentQueue<std::vector<byte>> verifyQueue_;
    BlockingConcurrentQueue<std::vector<byte>> processQueue_;
    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::thread processThread_;

    std::unique_ptr<Endpoint> endpoint_;

    // Replica state
    uint32_t instance_ = 1;   // in context of PBFT, this variable is the NEXT sequence number
    std::shared_ptr<Log> log_;
    std::shared_ptr<Application> app_;
    AppSnapshotStore appSnapshotStore_;

    // State for commit/checkpoint protocol
    CheckpointCollector checkpointCollector_;

    // State for fallback
    bool fallback_ = false;
    uint64_t fallbackStartTime_ = 0;
    uint64_t fallbackTriggerTime_ = 0;
    std::vector<std::pair<uint64_t, dombft::proto::ClientRequest>> fallbackQueuedReqs_;

    // fallback proposal is the current PBFT request
    std::optional<dombft::proto::RepairProposal> fallbackProposal_;
    byte proposalDigest_[SHA256_DIGEST_LENGTH];
    std::map<uint32_t, dombft::proto::RepairStart> fallbackHistorys_;
    std::map<uint32_t, std::string> fallbackHistorySigs_;
    std::optional<LogSuffix> fallbackProposalLogSuffix_;

    // State for PBFT
    bool viewChange_ = false;
    uint32_t pbftView_ = 0;                    // view num
    uint32_t preparedInstance_ = UINT32_MAX;   // Set to UINT32_MAX to indicate no prepared instance
    bool viewPrepared_ = true;
    PBFTState pbftState_;
    std::map<uint32_t, dombft::proto::PBFTPrepare> fallbackPrepares_;
    std::map<uint32_t, std::string> fallbackPrepareSigs_;
    std::map<uint32_t, dombft::proto::PBFTCommit> fallbackPBFTCommits_;
    std::map<uint32_t, dombft::proto::PBFTViewChange> pbftViewChanges_;
    std::map<uint32_t, std::string> pbftViewChangeSigs_;

    // State for actively triggering fallback and other testings
    uint32_t swapFreq_;
    uint32_t checkpointDropFreq_;
    std::optional<proto::ClientRequest> heldRequest_;

    // State for triggering view change
    uint32_t viewChangeFreq_;
    uint32_t viewChangeInst_;
    bool commitLocalInViewChange_ = false;   // when prepared, if send to itself a commit to try to go to next instance
    uint32_t viewChangeNum_;
    uint32_t viewChangeCounter_ = 0;
    // hold messages to cause timeout in which phase: true for commit, false for prepare, flip every view change
    bool holdPrepareOrCommit_ = false;

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

    void verifyMessagesThd();
    void processMessagesThd();

    void processClientRequest(const dombft::proto::ClientRequest &request);
    void processCert(const dombft::proto::Cert &cert);
    void processReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void processCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);
    void processSnapshotRequest(const dombft::proto::SnapshotRequest &snapshotRequest);
    void processSnapshotReply(const dombft::proto::SnapshotReply &snapshotReply);
    void processFallbackTrigger(const dombft::proto::FallbackTrigger &msg, std::span<byte> sig);
    void processRepairStart(const dombft::proto::RepairStart &msg, std::span<byte> sig);
    void checkTimeouts();

    bool verifyCert(const dombft::proto::Cert &cert);
    bool verifyFallbackProof(const dombft::proto::Cert &proof);
    bool verifyRepairProposal(const dombft::proto::RepairProposal &proposal);
    bool verifyViewChange(const dombft::proto::PBFTViewChange &viewChange);

    // Fallback Helpers
    void startFallback();
    void replyFromLogEntry(dombft::proto::Reply &reply, uint32_t seq);
    void finishFallback();
    void tryFinishFallback();
    void sendRepairSummaryToClients();
    LogSuffix &getFallbackLogSuffix();

    void holdAndSwapCliReq(const proto::ClientRequest &request);

    // TODO(Hao): test instance_== 0, seems problematic but a corner case
    inline bool ifTriggerViewChange() const
    {
        return !viewChange_ && instance_ != 0 && instance_ == viewChangeInst_ &&
               (viewChangeNum_ == 0 || viewChangeCounter_ < viewChangeNum_);
    }
    inline bool viewChangeByPrepare() const { return ifTriggerViewChange() && !holdPrepareOrCommit_; }
    inline bool viewChangeByCommit() const { return ifTriggerViewChange() && holdPrepareOrCommit_; }

    // fallback PBFT
    inline bool isPrimary() { return pbftView_ % replicaAddrs_.size() == replicaId_; }
    uint32_t getPrimary() { return pbftView_ % replicaAddrs_.size(); }
    void startViewChange();
    void doPrePreparePhase(uint32_t instance);
    void doPreparePhase();
    void doCommitPhase();
    void processPrePrepare(const dombft::proto::PBFTPrePrepare &msg);
    void processPrepare(const dombft::proto::PBFTPrepare &msg, std::span<byte> sig);
    void processPBFTCommit(const dombft::proto::PBFTCommit &msg);
    void processPBFTViewChange(const dombft::proto::PBFTViewChange &msg, std::span<byte> sig);
    void processPBFTNewView(const dombft::proto::PBFTNewView &msg);
    void getProposalDigest(byte *digest, const dombft::proto::RepairProposal &proposal);

    // sending helpers
    // note even though these are templates, we can define them in the cpp file because they are private
    // to this class.
    void sendSnapshotRequest(uint32_t replicaId, uint32_t targetSeq);
    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    Replica(
        const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq = 0, uint32_t viewChangeFreq = 0,
        bool commitLocalInViewChange = false, uint32_t viewChangeNum = 0, uint32_t checkpointDropFreq = 0
    );
    ~Replica();

    void run();
};

}   // namespace dombft