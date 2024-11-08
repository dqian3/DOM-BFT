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

struct CommitResInfo{
    bool committed = false;
    bool digest_updated = false;
};
class CheckpointCollector {
public:
    uint32_t replicaId_;
    uint32_t f_;
    uint32_t seq_;
    uint32_t instance_;
    std::optional<dombft::proto::Cert> cert_;
    std::optional<ClientRecords> clientRecords_;

    bool hasOwnReply_ = false;
    bool hasOwnCommit_ = false;

    std::map<uint32_t, dombft::proto::Reply> replies_;
    std::map<uint32_t, std::string> replySigs_;
    std::map<uint32_t, dombft::proto::Commit> commits_;
    std::map<uint32_t, std::string> commitSigs_;
    std::set<uint32_t> commitMatchedReplicas_;

    explicit CheckpointCollector(uint32_t replicaId, uint32_t f, uint32_t seq, uint32_t instance, std::optional<ClientRecords>& records):
    replicaId_(replicaId), f_(f), seq_(seq),instance_(instance), cert_(std::nullopt){
        clientRecords_ = std::move(records);
    }

    bool addAndCheckReplyCollection(const dombft::proto::Reply &reply, std::span<byte> sig);
    bool addAndCheckCommitCollection(const dombft::proto::Commit &commitMsg, const std::span<byte>& sig);
    CommitResInfo commitToLog(const std::shared_ptr<Log>& log, const dombft::proto::Commit &commit);

};

} // dombft

#endif //DOM_BFT_CHECKPOINT_H
