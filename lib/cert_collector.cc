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
    , round_(0)
{
}

size_t CertCollector::insertReply(Reply &reply, std::vector<byte> &&sig)
{
    int replicaId = reply.replica_id();

    // Note we only record replies by replicaId, not by round,
    // This means if we get replies from earlier rounds later from correct replicas,
    // we may not be able to collect a certificate
    replies_[replicaId] = reply;
    signatures_[replicaId] = std::move(sig);

    // If we receive f + 1 replies for a certain round, that means a correct
    // replica has advanced to that round, and we would only be able to
    // commit in that round.
    std::map<int, int> roundCounts;
    for (const auto &[replicaId, reply] : replies_) {
        roundCounts[reply.round()]++;

        if (roundCounts[reply.round()] >= f_ + 1) {
            if (reply.round() > round_) {
                round_ = reply.round();
                cert_.reset();
                VLOG(4) << "Increasing round for cert collector to " << round_;
            }
        }
    }

    // Try and find a certificate or proof of divergent histories
    std::map<ReplyKey, std::set<int>> matchingReplies;

    for (const auto &[replicaId, reply] : replies_) {
        if (reply.round() != round_) {
            continue;
        }

        ReplyKey key = {reply.seq(),        reply.round(),  reply.client_id(),
                        reply.client_seq(), reply.digest(), reply.result()};

        matchingReplies[key].insert(replicaId);

        maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());
        if (matchingReplies[key].size() >= 2 * f_ + 1) {

            // Skip creating certificate if we already have a certificate with a higher round
            if (cert_.has_value() && cert_->round() >= reply.round()) {
                continue;
            }

            cert_ = Cert();
            cert_->set_seq(std::get<0>(key));
            cert_->set_round(std::get<1>(key));

            for (auto repId : matchingReplies[key]) {
                std::string sigStr(signatures_[repId].begin(), signatures_[repId].end());
                cert_->add_signatures(sigStr);
                // THis usage is so weird, is protobuf the right tool?
                (*cert_->add_replies()) = replies_[repId];
            }
        }
    }

    if (VLOG_IS_ON(4)) {

        std::ostringstream oss;
        oss << "round=" << round_ << "\n";

        for (const auto &[replicaId, reply] : replies_) {
            dombft::apps::CounterResponse response;
            response.ParseFromString(reply.result());
            oss << replicaId << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.round()
                << " " << response.value() << "\n";
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

uint32_t CertCollector::numReceived() const
{
    uint32_t ret = 0;
    for (const auto &[replicaId, reply] : replies_) {
        if (reply.round() == round_) {
            ret++;
        }
    }
    return ret;
}