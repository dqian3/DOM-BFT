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

// size_t CertCollector::insertReply(Reply &reply, std::vector<byte> &&sig)
// {
//     int replicaId = reply.replica_id();

//     replies_[replicaId] = reply;
//     signatures_[replicaId] = sig;

//     typedef std::tuple<int, int, int, int, std::string, std::string> ReplyKey;

//     // Try and find a certificate or proof of divergent histories
//     std::map<ReplyKey, std::set<int>> matchingReplies;

//     for (const auto &[replicaId, reply] : replies_) {
//         // We also don't check the result here, that only needs to happen in the fast path
//         ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
//                         reply.client_seq(), reply.digest(),   reply.result()};

//         matchingReplies[key].insert(replicaId);
//         maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());

//         if (matchingReplies[key].size() >= 2 * f_ + 1) {
//             cert_ = Cert();
//             cert_->set_seq(std::get<0>(key));
//             cert_->set_instance(std::get<1>(key));

//             for (auto repId : matchingReplies[key]) {
//                 std::string sigStr(signatures_[repId].begin(), signatures_[repId].end());
//                 cert_->add_signatures(sigStr);
//                 // THis usage is so weird, is protobuf the right tool?
//                 (*cert_->add_replies()) = replies_[repId];
//             }
//         }
//     }

//     if (VLOG_IS_ON(4)) {
//         std::ostringstream oss;
//         oss << "\n";

//         // TODO this is just for logging,
//         dombft::apps::CounterResponse response;
//         response.ParseFromString(reply.result());

//         for (const auto &[replicaId, reply] : replies_) {
//             oss << replicaId << " " << digest_to_hex(reply.digest()).substr(56) << " " << reply.seq() << " "
//                 << reply.instance() << " " << response.value() << "\n";
//         }

//         std::string logOutput = oss.str();
//         VLOG(4) << logOutput;
//     }

//     return maxMatchSize_;
// }

size_t CertCollector::insertBatchedReply(uint32_t replicaId, BatchedReply &batchedReply, std::vector<byte> &&sig)
{
    CertEntry certEntry;
    certEntry.set_replica_id(replicaId);
    *certEntry.mutable_batched_reply() = batchedReply;
    certEntry.set_signature(std::string(sig.begin(), sig.end()));

    certEntries_[replicaId] = certEntry;

    // Now, we need to find matching replies in the batched replies.
    // For simplicity, let's assume we are matching based on a specific client sequence number.

    typedef std::tuple<uint32_t, std::string> ReplyKey; // (client_seq, digest)

    std::map<ReplyKey, std::set<uint32_t>> matchingReplies;

    for (const auto &[repId, entry] : certEntries_) {
        const BatchedReply &br = entry.batched_reply();

        for (const Reply &reply : br.replies()) {
            ReplyKey key = {reply.client_seq(), reply.digest()};

            matchingReplies[key].insert(repId);

            size_t matchSize = matchingReplies[key].size();
            maxMatchSize_ = std::max(maxMatchSize_, matchSize);

            if (matchSize >= 2 * f_ + 1) {


                cert_ = Cert();
                cert_->set_seq(reply.seq());
                cert_->set_instance(reply.instance());
                cert_->set_client_id(reply.client_id());
                // for now do not worry about the start and end idx. 
                // cert_->set_batch_start_seq(br.batch_start_seq());
                // cert_->set_batch_end_seq(br.batch_end_seq());

                // the batched signature itself is contained in a cert entry
                for (uint32_t matchedReplicaId : matchingReplies[key]) {
                    *cert_->add_cert_entries() = certEntries_[matchedReplicaId];
                }

                LOG(INFO) << "CERT formed for client_seq=" << reply.client_seq() << " seq=" << reply.seq()
                          << " instance=" << reply.instance() << " client id " << reply.client_id();
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