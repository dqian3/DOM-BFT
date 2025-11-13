#ifndef DOM_BFT_CHECKPOINT_H
#define DOM_BFT_CHECKPOINT_H

#include "lib/client_record.h"
#include "lib/common.h"
#include "lib/log.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <glog/logging.h>
#include <optional>
#include <span>

typedef std::tuple<std::string, uint32_t, uint32_t> ReplyKeyTuple;

struct ReplyCollector {
    uint32_t replicaId_;

    uint32_t round_;
    uint32_t seq_;

    bool hasOwnReply_ = false;

    std::map<uint32_t, dombft::proto::Reply> replies_;
    std::map<uint32_t, std::string> replySigs_;
    std::optional<dombft::proto::Cert> cert_;

    ReplyCollector(uint32_t replicaId, uint32_t round, uint32_t seq)
        : replicaId_(replicaId)
        , round_(round)
        , seq_(seq)
    {
    }

    bool addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig);
};

typedef std::tuple<uint32_t, uint32_t, std::string, std::string, std::string> CommitKeyTuple;

struct CommitCollector {
    uint32_t round_;
    uint32_t seq_;

    std::optional<dombft::proto::Commit> commitToUse_;

    std::map<uint32_t, dombft::proto::Commit> commits_;
    std::map<uint32_t, std::string> sigs_;
    std::set<uint32_t> matchedReplicas_;

    CommitCollector(uint32_t round, uint32_t seq)
        : round_(round)
        , seq_(seq)
    {
    }

    bool addAndCheckCommit(const dombft::proto::Commit &commitMsg, const std::span<byte> sig);
    void getCheckpoint(::LogCheckpoint &checkpoint) const;
};

class CheckpointCollector {
    // We only need our own latest state, so don't index by round

    uint32_t replicaId_;
    uint32_t round_;
    uint32_t seq_;

    bool timeout_ = false;
    uint64_t timeoutStart_ = 0;

    bool needsSnapshot_ = false;
    std::optional<AppSnapshot> snapshot_;
    std::optional<::ClientRecord> clientRecord_;
    std::optional<std::string> logDigest_;

    std::optional<dombft::proto::RepairTimeoutProof> repairTimeoutProof_;

    // Messages
    ReplyCollector replyCollector;
    CommitCollector commitCollector;

    std::map<uint32_t, dombft::proto::RepairTimeout> repairTimeouts_;
    std::map<uint32_t, std::string> repairTimeoutSigs_;

public:
    explicit CheckpointCollector(uint32_t replicaId, uint32_t round, uint32_t seq, bool needsSnapshot)
        : replicaId_(replicaId)
        , round_(round)
        , seq_(seq)
        , needsSnapshot_(needsSnapshot)
        , replyCollector(replicaId, round, seq)
        , commitCollector(round, seq)
    {
    }

    bool needsSnapshot() const { return needsSnapshot_; }

    // Add a reply to the collector and check if we have enough replies to form a cert
    // Can call getCert once addAndCheckReply returns true
    bool addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    bool hasCert() const { return replyCollector.cert_.has_value(); }
    void getCert(dombft::proto::Cert &cert) const { cert = replyCollector.cert_.value(); }

    // If during a checkpoint round, we took a snapshot of our own state (optional)
    // It is needed for new commit messages if hasAppSnapshot is true, since we need to update
    // the commit messages
    void addOwnSnapshot(const AppSnapshot &snapshot);
    bool hasOwnSnapshot() const { return snapshot_.has_value(); }

    void addOwnState(const std::string &logDigest, const ::ClientRecord &clientRecord);
    bool hasOwnState() const { return clientRecord_.has_value() && logDigest_.has_value(); }

    // Commit phase is ready once we have a cert and own own state/snapshot
    bool commitReady() const;
    void getOwnCommit(dombft::proto::Commit &commit) const;

    // Add a reply to the collector and check if we have enough replies to form a cert
    // Can call getCheckpoint once addAndCheckCommit returns true
    bool addAndCheckCommit(const dombft::proto::Commit &commit, std::span<byte> sig);

    // Note checkpoint will only have snapshot field set to valid pointer if we have
    // the pointer from our own log
    void getCheckpoint(::LogCheckpoint &checkpoint) const;

    bool addAndCheckTimeout(const dombft::proto::RepairTimeout &timeoutMsg, std::span<byte> sig);

    bool checkSelfTimeout(uint64_t now, uint64_t timeoutMs);
    void getRepairTimeoutProof(dombft::proto::RepairTimeoutProof &timeoutProof) const;
};

class CheckpointCollectorStore {

    uint32_t stableSeq_ = 0;
    uint32_t committedSeq_ = 0;

    uint32_t replicaId_;

public:
    std::map<std::pair<uint32_t, uint32_t>, CheckpointCollector> collectors_;
    std::map<std::pair<uint32_t, uint32_t>, CheckpointCollector> snapshotCollectors_;

    explicit CheckpointCollectorStore(uint32_t replicaId)
        : replicaId_(replicaId)
    {
    }

    bool initCollector(uint32_t round, uint32_t seq, bool needsSnapshot);
    bool hasCollector(uint32_t round, uint32_t seq);

    CheckpointCollector &at(uint32_t round, uint32_t seq);

    void cleanStaleCollectors(uint32_t stableSeq, uint32_t committedSeq);
};

#endif   // DOM_BFT_CHECKPOINT_H
