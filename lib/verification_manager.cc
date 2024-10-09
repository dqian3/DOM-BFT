#include "verification_manager.h"
#include <glog/logging.h>

VerificationManager::VerificationManager(uint32_t f, SignatureProvider& sigProvider, size_t threadPoolSize)
    : f_(f), sigProvider_(sigProvider), threadPoolSize_(threadPoolSize), stop_(false) {
    for (size_t i = 0; i < threadPoolSize_; ++i) {
        workers_.emplace_back(&VerificationManager::workerThread, this);
    }
}

VerificationManager::~VerificationManager() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        worker.join();
    }
}


void VerificationManager::workerThread() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}


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


    // Verify each signature in the cert in parallel
    std::vector<std::future<bool>> futures;
    for (size_t i = 0; i < cert.replies().size(); i++) {
        const dombft::proto::Reply& reply = cert.replies()[i];
        const std::string& sig = cert.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();

        futures.push_back(enqueueTask([this, serializedReply, sig, &reply]() {
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
            return true;
        }));
    }

    for (auto& future : futures) {
        if (!future.get()) {
            return false;
        }
    }

    // Verify each signature in the cert
    std::set<int> replicaIds; // To check for duplicate signatures
    for (size_t i = 0; i < cert.replies().size(); i++) {
        const dombft::proto::Reply& reply = cert.replies()[i];
        
        if (!replicaIds.insert(reply.replica_id()).second) {
            LOG(INFO) << "Duplicate signature from replica " << reply.replica_id();
            return false;
        }
    }
    return true;
}


bool VerificationManager::verifyReply(const dombft::proto::Reply& reply, const std::string& signature) {
    std::string serializedReply = reply.SerializeAsString();

    auto future = enqueueTask([this, serializedReply, signature, &reply]() {
        if (!sigProvider_.verify(
                (byte*)serializedReply.c_str(),
                serializedReply.size(),
                (byte*)signature.c_str(),
                signature.size(),
                "replica",
                reply.replica_id())) {
            LOG(INFO) << "Failed to verify reply signature for replica " << reply.replica_id();
            return false;
        }
        LOG(INFO) << "Reply verified for replica " << reply.replica_id();
        return true;
    });

    return future.get();
}

bool VerificationManager::verifyFallbackTrigger(const dombft::proto::FallbackTrigger& trigger) {
    const dombft::proto::Cert& proof = trigger.proof();

    // Verify that the proof contains enough replies to prove inconsistency
    if (proof.replies().size() < 2 * f_ + 1) {
        LOG(INFO) << "Fallback trigger proof contains insufficient replies. Size: " << proof.replies().size()
                  << ", required: " << 2 * f_ + 1;
        return false;
    }

    // Verify each reply in the proof in parallel
    std::vector<std::future<bool>> futures;
    for (size_t i = 0; i < proof.replies().size(); i++) {
        const dombft::proto::Reply& reply = proof.replies()[i];
        const std::string& sig = proof.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();

        futures.push_back(enqueueTask([this, serializedReply, sig, &reply]() {
            if (!sigProvider_.verify(
                    (byte*)serializedReply.c_str(),
                    serializedReply.size(),
                    (byte*)sig.c_str(),
                    sig.size(),
                    "replica",
                    reply.replica_id())) {
                LOG(INFO) << "Fallback trigger proof failed to verify for replica " << reply.replica_id();
                return false;
            }
            return true;
        }));
    }

    for (auto& future : futures) {
        if (!future.get()) {
            return false;
        }
    }

    LOG(INFO) << "Fallback trigger proof verified successfully";
    return true;
}