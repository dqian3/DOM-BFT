#include "lib/cert_collector.h"
#include "lib/utils.h"

#include "proto/dombft_apps.pb.h"

#include <glog/logging.h>

#include <iostream>
#include <map>
#include <sstream>
#include <tuple>

using namespace dombft::proto;

CertCollector::CertCollector(int f)
    : f_(f)
    , maxMatchSize_(0)
{
}

int CertCollector::insertReply(Reply &reply, std::vector<byte> &&sig)
{
    int replicaId = reply.replica_id();

    replies_[replicaId] = reply;
    signatures_[replicaId] = sig;

    typedef std::tuple<int, int, int, int, std::string, std::string> ReplyKey;

    // Try and find a certificate or proof of divergent histories
    std::map<ReplyKey, std::set<int>> matchingReplies;

    if (VLOG_IS_ON(4)) {
        std::ostringstream oss;
        oss << "\n";

        dombft::apps::CounterResponse response;
        response.ParseFromString(reply.result());

        for (const auto &[replicaId, reply] : replies_) {
            oss << digest_to_hex(reply.digest()).substr(56) << " " << reply.seq() << " " << reply.instance() << "\n";
        }

        std::string logOutput = oss.str();
        VLOG(4) << logOutput;
    }

    for (const auto &[replicaId, reply] : replies_) {
        // We also don't check the result here, that only needs to happen in the fast path
        // TODO figure out exactly what to do with the instance here, right now it's always 0
        ReplyKey key = {reply.seq(), 0, reply.client_id(), reply.client_seq(), reply.digest(), reply.result()};

        matchingReplies[key].insert(replicaId);
        maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());

        if (matchingReplies[key].size() >= 2 * f_ + 1) {
            cert_ = Cert();
            cert_->set_seq(std::get<0>(key));
            cert_->set_instance(std::get<1>(key));

            // TODO check if fast path is not posssible, and we can send cert right away
            for (auto repId : matchingReplies[key]) {
                std::string sigStr(signatures_[repId].begin(), signatures_[repId].end());
                cert_->add_signatures(sigStr);
                // THis usage is so weird, is protobuf the right tool?
                (*cert_->add_replies()) = replies_[repId];
            }
        }
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