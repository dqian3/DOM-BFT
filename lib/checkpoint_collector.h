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
typedef std::tuple<std::string, std::string, uint32_t, uint32_t, std::string> CommitKeyTuple;

class CheckpointCollector {
public:
    uint32_t replicaId_;
    uint32_t f_;
    uint32_t seq_;
    uint32_t instance_;

    std::optional<dombft::proto::Cert> cert_;
    bool hasOwnReply_ = false;

    std::optional<dombft::proto::Commit> commitToUse_;
    std::optional<ClientRecord> clientRecord_;

    std::map<uint32_t, dombft::proto::Reply> replies_;
    std::map<uint32_t, std::string> replySigs_;
    std::map<uint32_t, dombft::proto::Commit> commits_;
    std::map<uint32_t, std::string> commitSigs_;
    std::set<uint32_t> commitMatchedReplicas_;

    explicit CheckpointCollector(
        uint32_t replicaId, uint32_t f, uint32_t seq, uint32_t instance, std::optional<ClientRecord> &records
    )
        : replicaId_(replicaId)
        , f_(f)
        , seq_(seq)
        , instance_(instance)
        , cert_(std::nullopt)
    {
        clientRecord_ = std::move(records);
    }

    bool addAndCheckReplyCollection(const dombft::proto::Reply &reply, std::span<byte> sig);
    bool addAndCheckCommitCollection(const dombft::proto::Commit &commitMsg, const std::span<byte> sig);

    void getCheckpoint(LogCheckpoint &checkpoint);
};

class CheckpointCollectors {
    std::map<uint32_t, CheckpointCollector> collectors_;
    uint32_t replicaId_;
    uint32_t f_;

public:
    explicit CheckpointCollectors(uint32_t replicaId, uint32_t f)
        : replicaId_(replicaId)
        , f_(f)
    {
    }

    inline CheckpointCollector &at(uint32_t seq) { return collectors_.at(seq); }
    inline bool has(uint32_t seq) { return collectors_.find(seq) != collectors_.end(); }
    void tryInitCheckpointCollector(uint32_t seq, uint32_t instance, std::optional<ClientRecord> &&records);
    void cleanSkippedCheckpointCollectors(uint32_t committedSeq, uint32_t committedInstance);
};

#endif   // DOM_BFT_CHECKPOINT_H
