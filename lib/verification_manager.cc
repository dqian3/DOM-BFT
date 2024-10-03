#include "verification_manager.h"
#include <glog/logging.h>

VerificationManager::VerificationManager(uint32_t f, SignatureProvider& sigProvider)
    : f_(f), sigProvider_(sigProvider) {}


bool VerificationManager::verifyCert(const dombft::proto::Cert& cert) {
    if (cert.replies().size() < 2 * f_ + 1) {
        LOG(INFO) << "Received cert of size " << cert.replies().size()
                  << ", which is smaller than 2f + 1, f=" << f_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size()
                  << " is not equal to cert signatures size " << cert.signatures().size();
        return false;
    }

    // TODO: Check cert instance

    // Verify each signature in the cert
    std::set<int> replicaIds; // To check for duplicate signatures
    for (size_t i = 0; i < cert.replies().size(); i++) {
        const dombft::proto::Reply& reply = cert.replies()[i];
        const std::string& sig = cert.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();

        if (!sigProvider_.verify(
                (byte*)serializedReply.c_str(),
                serializedReply.size(),
                (byte*)sig.c_str(),
                sig.size(),
                "replica",
                reply.replica_id())) {
            LOG(INFO) << "Cert failed to verify for replica " << reply.replica_id();
            return false;
        }

        // Check for duplicate signatures
        if (!replicaIds.insert(reply.replica_id()).second) {
            LOG(INFO) << "Duplicate signature from replica " << reply.replica_id();
            return false;
        }
    }

    // Additional verification steps...

    return true;
}