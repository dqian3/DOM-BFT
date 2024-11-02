#ifndef DOM_BFT_CHECKPOINT_H
#define DOM_BFT_CHECKPOINT_H

#include "lib/common.h"
#include "lib/utils.h"
#include "lib/log.h"
#include "lib/client_record.h"
#include "proto/dombft_proto.pb.h"

#include <optional>
#include <span>
#include <glog/logging.h>

namespace dombft {
using namespace dombft::proto;

typedef std::tuple<std::string, uint32_t, uint32_t> ReplyKeyTuple;
typedef std::tuple<std::string, std::string, uint32_t, uint32_t, std::string> CommitKeyTuple;
class CheckpointCollector {
public:
    bool inProgress = false;
    uint32_t replicaId_;
    uint32_t f_;
    // inclusive range
    uint32_t startSeq_;
    uint32_t endSeq_;
    std::optional<dombft::proto::Cert> cert_;
    ClientRecords clientRecords_;

    std::map<uint32_t, dombft::proto::Reply> replies_;
    std::map<uint32_t, std::string> replySigs_;
    std::map<uint32_t, dombft::proto::Commit> commits_;
    std::map<uint32_t, std::string> commitSigs_;
    std::set<uint32_t> commitMatchedReplicas_;

    explicit CheckpointCollector(uint32_t replicaId, uint32_t f): replicaId_(replicaId), f_(f) {}

    void checkpointStart(uint32_t start_seq, uint32_t end_seq, ClientRecords& records);
    bool addAndCheckReplyCollection(const dombft::proto::Reply &reply, std::span<byte> sig);
    bool addAndCheckCommitCollection(const dombft::proto::Commit &commitMsg, std::span<byte> sig);

};

} // dombft

#endif //DOM_BFT_CHECKPOINT_H
