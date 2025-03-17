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
    , instance_(0)
{
}

size_t CertCollector::insertReply(Reply &reply, std::vector<byte> &&sig)
{
    int replicaId = reply.replica_id();

    // Note we only record replies by replicaId, not by instance,
    // This means if we get replies from earlier instances later from correct replicas,
    // we may not be able to collect a certificate
    replies_[replicaId] = reply;
    signatures_[replicaId] = std::move(sig);

    // If we receive f + 1 replies for a certain instance, that means a correct
    // replica has advanced to that instance, and we would only be able to
    // commit in that instance.
    std::map<int, int> instanceCounts;
    for (const auto &[replicaId, reply] : replies_) {
        instanceCounts[reply.instance()]++;

        if (instanceCounts[reply.instance()] >= f_ + 1) {
            if (reply.instance() > instance_) {
                instance_ = reply.instance();
                cert_.reset();
                VLOG(4) << "Increasing instance for cert collector to " << instance_;
            }
        }
    }

    // Try and find a certificate or proof of divergent histories
    std::map<ReplyKey, std::set<int>> matchingReplies;

    for (const auto &[replicaId, reply] : replies_) {
        if (reply.instance() != instance_) {
            continue;
        }

        ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
                        reply.client_seq(), reply.digest(),   reply.result()};

        matchingReplies[key].insert(replicaId);

        maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());
        if (matchingReplies[key].size() >= 2 * f_ + 1) {

            // Skip creating certificate if we already have a certificate with a higher instance
            if (cert_.has_value() && cert_->instance() >= reply.instance()) {
                continue;
            }

            cert_ = Cert();
            cert_->set_seq(std::get<0>(key));
            cert_->set_instance(std::get<1>(key));

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
        oss << "instance=" << instance_ << "\n";

        for (const auto &[replicaId, reply] : replies_) {
            dombft::apps::CounterResponse response;
            response.ParseFromString(reply.result());
            oss << replicaId << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.instance()
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
        if (reply.instance() == instance_) {
            ret++;
        }
    }
    return ret;
}