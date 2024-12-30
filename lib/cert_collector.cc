#include "lib/cert_collector.h"
#include "lib/utils.h"

#include "proto/dombft_apps.pb.h"

#include <glog/logging.h>

#include <iostream>
#include <map>
#include <sstream>
#include <tuple>
#include <span>

using namespace dombft::proto;

CertCollector::CertCollector(uint32_t f)
    : f_(f)
    , maxMatchSize_(0)
{
}

size_t CertCollector::insertReply(Reply &reply, std::span<byte> &sig,const std::shared_ptr<dombft::proto::BatchedReply>& batchedReply)
{
    uint32_t replicaId = reply.replica_id();

    replies_[replicaId] = {reply,batchedReply};
    signatures_[replicaId] = {sig.begin(), sig.end()};

    // Try and find a certificate or proof of divergent histories
    std::map<ReplyKey, std::set<int>> matchingReplies;

    for (const auto &[replicaId, replyAndBatch] : replies_) {
        const Reply &reply = replyAndBatch.first;
        const std::shared_ptr<dombft::proto::BatchedReply>& batch = replyAndBatch.second;
        // We also don't check the result here, that only needs to happen in the fast path
        ReplyKey key = {reply.seq(),    reply.instance(), reply.client_id(), reply.client_seq(),
                        reply.digest(), reply.result(),   reply.retry()};

        matchingReplies[key].insert(replicaId);
        maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());
        if (matchingReplies[key].size() >= 2 * f_ + 1) {
            cert_ = Cert();
            cert_->set_seq(std::get<0>(key));
            cert_->set_instance(std::get<1>(key));

            for (auto repId : matchingReplies[key]) {
                std::string sigStr(signatures_[repId].begin(), signatures_[repId].end());
                cert_->add_signatures(sigStr);

                ReplyAndBatch *rAb = cert_->add_replies();
                rAb->mutable_reply()->CopyFrom(reply);
                if(batch!=nullptr) { rAb->mutable_batch()->CopyFrom(*batch); }
            }
        }
    }

    if (VLOG_IS_ON(4)) {
        std::ostringstream oss;
        oss << "\n";

        // TODO this is just for logging, make it work for kv_store
        for (const auto &[replicaId, replyAndBatch] : replies_) {
            const Reply &reply = replyAndBatch.first;
            dombft::apps::CounterResponse response;
            response.ParseFromString(reply.result());
            oss << replicaId << " " << digest_to_hex(reply.digest()).substr(56) << " " << reply.seq() << " "
                << reply.instance() << " " << response.value() << "\n";
        }

        std::string logOutput = oss.str();
        VLOG(4) << logOutput;
    }

    return maxMatchSize_;
}

bool CertCollector::hasCert() { return cert_.has_value(); }

const dombft::proto::Cert &CertCollector::getCert()
{
    if (!hasCert()) {
        throw std::logic_error("Called getCert() while hasCert() is false!");
    }

    return cert_.value();
}