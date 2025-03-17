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
    uint32_t f_;
    uint32_t round_;
    uint32_t seq_;

    bool hasOwnReply_ = false;

    std::map<uint32_t, dombft::proto::Reply> replies_;
    std::map<uint32_t, std::string> replySigs_;
    std::optional<dombft::proto::Cert> cert_;

    ReplyCollector(uint32_t replicaId, uint32_t f, uint32_t seq, uint32_t round)
        : replicaId_(replicaId)
        , f_(f)
        , round_(round)
        , seq_(seq)
    {
    }

    bool addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    void getCert(dombft::proto::Cert &cert);
};

typedef std::tuple<std::string, std::string, uint32_t, uint32_t, std::string> CommitKeyTuple;

struct CommitCollector {
    uint32_t f_;
    uint32_t round_;

    uint32_t seq_;

    std::optional<dombft::proto::Commit> commitToUse_;

    std::map<uint32_t, dombft::proto::Commit> commits_;
    std::map<uint32_t, std::string> sigs_;
    std::set<uint32_t> matchedReplicas_;

    CommitCollector(uint32_t f, uint32_t round, uint32_t seq)
        : f_(f)
        , round_(round)
        , seq_(seq)
    {
    }

    bool addAndCheckCommit(const dombft::proto::Commit &commitMsg, const std::span<byte> sig);
    void getCheckpoint(::LogCheckpoint &checkpoint);
};

// Cached state of the log (snapshot, client record, digest) that the replica may need to
// finish checkpointing
struct CheckpointState {
    std::string logDigest;
    ::ClientRecord clientRecord_;

    AppSnapshot snapshot;
};

class CheckpointCollector {
    // Each collector is indexed by (round, key)
    std::map<std::pair<uint32_t, uint32_t>, ReplyCollector> replyCollectors_;
    std::map<std::pair<uint32_t, uint32_t>, CommitCollector> commitCollectors_;

    // We only need our own latests state, so don't index by round
    std::map<uint32_t, CheckpointState> states_;

    uint32_t replicaId_;
    uint32_t f_;

public:
    explicit CheckpointCollector(uint32_t replicaId, uint32_t f)
        : replicaId_(replicaId)
        , f_(f)
    {
    }

    bool addAndCheckReply(const dombft::proto::Reply &reply, std::span<byte> sig);
    bool addAndCheckCommit(const dombft::proto::Commit &commit, std::span<byte> sig);

    void getCert(uint32_t round, uint32_t seq, dombft::proto::Cert &cert);

    void getCommitToUse(uint32_t round, uint32_t seq, dombft::proto::Commit &commit);
    void getCheckpoint(uint32_t round, uint32_t seq, ::LogCheckpoint &checkpoint);

    const CheckpointState &getCachedState(uint32_t seq);

    void
    cacheState(uint32_t seq, const std::string &logDigest, const ::ClientRecord &clientRecord, AppSnapshot &&snapshot);

    void cleanStaleCollectors(uint32_t committedSeq, uint32_t committedRound);
};

#endif   // DOM_BFT_CHECKPOINT_H
