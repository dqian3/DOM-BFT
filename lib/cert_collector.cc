#include <cert_collector.h>

#include <glog/logging.h>

#include <map>
#include <tuple>

CertCollector::CertCollector(int f)
    : f_(f)
    , maxMatchSize_(0)
{
}

int CertCollector::insertReply(dombft::proto::Reply &reply, std::vector<byte> &sig)
{
    int replicaId = reply.replica_id();

    replies_[replicaId] = std::move(reply);
    signatures_[replicaId] == std::(sig);

    typedef std::tuple<int, int, int, int, std::string, std::string> ReplyKey;

    // Try and find a certificate or proof of divergent histories
    std::map<ReplyKey, std::set<int>> matchingReplies;

    for (const auto &entry : replies) {
        int replicaId = entry.first;
        const Reply &reply = entry.second;

        // We also don't check the result here, that only needs to happen in the fast path
        ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
                        reply.client_seq(), reply.digest(),   reply.result()};

        matchingReplies[key].insert(replicaId);
        maxMatchSize_ = std::max(maxMatchSize_, matchingReplies[key].size());

        if (matchingReplies[key].size() >= 2 * f_ + 1) {
            cert_ = Cert();
            cert->set_seq(std::get<0>(key));
            cert->set_instance(std::get<1>(key));

            // TODO check if fast path is not posssible, and we can send cert right away
            for (auto repId : matchingReplies[key]) {
                cert_->add_signatures(reqState.signatures[repId]);
                // THis usage is so weird, is protobuf the right tool?
                (*cert_->add_replies()) = reqState.replies[repId];
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

bool verifyCert(const Cert &cert, int f)
{
    if (cert.replies().size() < 2 * f + 1) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to "
                  << "cert signatures size" << cert.signatures().size();
        return false;
    }

    // Verify each signature in the cert
    for (int i = 0; i < cert.replies().size(); i++) {
        const Reply &reply = cert.replies()[i];
        const std::string &sig = cert.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();   // TODO skip reseraizliation here?

        if (!sigProvider_.verify((byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(),
                                 sig.size(), "replica", reply.replica_id())) {
            LOG(INFO) << "Cert failed to verify!";
            return false;
        }
    }

    // Create map

    // TOOD verify that cert actually contains matching replies...
    // And that there aren't signatures from the same replica.

    return true;
}
