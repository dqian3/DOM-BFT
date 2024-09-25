#include "client.h"

#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "proto/dombft_apps.pb.h"

#define NUM_CLIENTS 100

namespace dombft {
using namespace dombft::proto;

Client::Client(const ProcessConfig &config, size_t id)
    : clientId_(id)
{
    LOG(INFO) << "clientId=" << clientId_;
    std::string clientIp = config.clientIps[clientId_];
    LOG(INFO) << "clientIp=" << clientIp;
    int clientPort = config.clientPort;
    LOG(INFO) << "clientPort=" << clientPort;

    f_ = config.replicaIps.size() / 3;
    normalPathTimeout_ = config.clientNormalPathTimeout;
    slowPathTimeout_ = config.clientSlowPathTimeout;

    LOG(INFO) << "Running for " << config.clientRuntimeSeconds << " seconds";

    LOG(INFO) << "Sending at most " << config.clientMaxInFlight << " requests at once";

    maxInFlight_ = config.clientMaxInFlight;
    sendRate_ = config.clientSendRate;

    if (config.clientSendMode == "sendRate") {
        sendMode_ = dombft::RateBased;
        LOG(INFO) << "Send rate: " << sendRate_;
    } else if (config.clientSendMode == "maxInFlight") {
        sendMode_ = dombft::MaxInFlightBased;
    }

    /* Setup keys */
    std::string clientKey = config.clientKeysDir + "/client" + std::to_string(clientId_) + ".pem";
    LOG(INFO) << "Loading key from " << clientKey;
    if (!sigProvider_.loadPrivateKey(clientKey)) {
        LOG(ERROR) << "Error loading client private key, exiting...";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("replica", config.replicaKeysDir)) {
        LOG(ERROR) << "Error loading replica public keys, exiting...";
        exit(1);
    }

    /** Setup transport */
    if (config.transport == "nng") {
        auto addrPairs = getClientAddrs(config, clientId_);

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true);

        for (size_t i = 0; i < addrPairs.size(); i++) {
        }

        size_t nReplicas = config.replicaIps.size();
        for (size_t i = 0; i < nReplicas; i++)
            replicaAddrs_.push_back(addrPairs[i].second);

        for (size_t i = nReplicas; i < addrPairs.size(); i++)
            proxyAddrs_.push_back(addrPairs[i].second);
    } else {
        endpoint_ = std::make_unique<UDPEndpoint>(clientIp, clientPort, true);

        /** Store all proxy addrs. TODO handle mutliple proxy sockets*/
        for (uint32_t i = 0; i < config.proxyIps.size(); i++) {
            LOG(INFO) << "Proxy " << i + 1 << ": " << config.proxyIps[i] << ", " << config.proxyForwardPort;
            proxyAddrs_.push_back(Address(config.proxyIps[i], config.proxyForwardPort));
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
        }
    }

    /** Initialize state */
    nextSeq_ = 1;

    MessageHandlerFunc replyHandler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
    };

    endpoint_->RegisterMsgHandler(replyHandler);

    timeoutTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Client *) ctx)->checkTimeouts(); }, 5000, this);

    endpoint_->RegisterTimer(timeoutTimer_.get());

    terminateTimer_ = std::make_unique<Timer>(
        [config](void *ctx, void *endpoint) {
            LOG(INFO) << "Exiting  after running for " << config.clientRuntimeSeconds << " seconds";
            // TODO print some stats
            exit(0);
        },
        config.clientRuntimeSeconds * 1000000,   // timer is in us.
        this);

    // Set high priority (lower is more priority) to terminate properly.
    ev_set_priority(terminateTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(terminateTimer_.get());

    if (config.app == AppType::COUNTER) {
        trafficGen_ = std::make_unique<CounterTrafficGen>();
        appType_ = AppType::COUNTER;
    } else {
        LOG(ERROR) << "Unknown application type for client!";
        exit(1);
    }

    if (sendMode_ == dombft::RateBased) {

        sendTimer_ = std::make_unique<Timer>(
            [&](void *ctx, void *endpoint) {
                // If we are in the slow path, don't submit anymore
                if (std::max(lastFastPath_, lastNormalPath_) < lastSlowPath_ && numInFlight_ >= 1) {
                    VLOG(6) << "Pause sending because slow path: lastFastPath_=" << lastFastPath_
                            << " lastNormalPath_=" << lastNormalPath_ << " lastSlowPath_=" << lastSlowPath_
                            << " numInFlight=" << numInFlight_;

                    return;
                }

                submitRequest();

                // If we are in the normal path, make the send rate slower by a factor of n as a way to slow down
                if (lastFastPath_ < lastNormalPath_) {
                    endpoint_->ResetTimer(sendTimer_.get(), replicaAddrs_.size() * 1000000 / sendRate_);
                } else {
                    endpoint_->ResetTimer(sendTimer_.get(), 1000000 / sendRate_);
                }
            },
            1000000 / sendRate_, this);

        endpoint_->RegisterTimer(sendTimer_.get());
    } else if (sendMode_ == dombft::MaxInFlightBased) {
        for (uint32_t i = 0; i < maxInFlight_; i++) {
            submitRequest();
        }
    } else {
        LOG(ERROR) << "Unknown send mode type for client!";
        exit(1);
    }

    LOG(INFO) << "Client finished initializing";
    endpoint_->LoopRun();
}

Client::~Client()
{
    // TODO cleanup... though we don't really reuse this
}

void Client::submitRequest()
{
    ClientRequest request;

    uint64_t now = GetMicrosecondTimestamp();

    // submit new request
    request.set_client_id(clientId_);
    request.set_client_seq(nextSeq_);
    request.set_instance(myInstance_);
    request.set_send_time(now);
    request.set_is_write(true);   // TODO modify this based on some random chance

    auto appRequest = trafficGen_->generateAppTraffic();
    // TODO: this has to be hard coded in an inelegant way. May imporve this later
    if (appType_ == AppType::COUNTER) {
        dombft::apps::CounterRequest *counterReq = (dombft::apps::CounterRequest *) appRequest;
        request.set_req_data(counterReq->SerializeAsString());
    }

    requestStates_.emplace(nextSeq_, RequestState(f_, request, now));
    sendRequest(request);

    VLOG(1) << "PERF event=send" << " client_id=" << clientId_ << " client_seq=" << nextSeq_
            << " in_flight=" << numInFlight_;

    nextSeq_++;
    numInFlight_++;
}

void Client::sendRequest(const ClientRequest &request)
{
#if USE_PROXY
    // TODO how to choose proxy, perhaps by IP or config
    // VLOG(4) << "Begin sending request number " << nextReqSeq_;
    Address &addr = proxyAddrs_[clientId_ % proxyAddrs_.size()];
    // TODO maybe client should own the memory instead of endpoint.
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
    // VLOG(4) << "Serialization Done " << nextReqSeq_;
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    // VLOG(4) << "Signature Done " << nextReqSeq_;

    endpoint_->SendPreparedMsgTo(addr);
#else
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
    // TODO check errors for all of these lol
    // TODO do this while waiting, not in the critical path
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr);
    }
#endif
}

void Client::commitRequest(uint32_t clientSeq)
{
    // TODO inform application of result
    if (clientSeq > lastCommitted_ + 1) {
        LOG(WARNING) << "Committed out of order! Commited " << clientSeq << " after committing " << lastCommitted_;
    }
    lastCommitted_ = clientSeq;

    requestStates_.erase(clientSeq);
    numCommitted_++;
    numInFlight_--;

    VLOG(2) << "After committing, numInFlight_=" << numInFlight_;

    if (sendMode_ == dombft::MaxInFlightBased) {
        submitRequest();
    }
}

void Client::checkTimeouts()
{
    uint64_t now = GetMicrosecondTimestamp();

    for (auto &entry : requestStates_) {
        int clientSeq = entry.first;
        RequestState &reqState = entry.second;
        if (reqState.collector.hasCert() && !reqState.certSent && now - reqState.certTime > normalPathTimeout_) {
            VLOG(2) << "Request number " << clientSeq << " fast path timed out! Sending cert!";
            reqState.certSent = true;

            lastNormalPath_ = clientSeq;

            // Send cert to replicas;
            endpoint_->PrepareProtoMsg(reqState.collector.getCert(), CERT);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }
            continue;
        }

        if (!reqState.triggerSent && now - reqState.sendTime > slowPathTimeout_) {
            LOG(INFO) << "Client attempting fallback on request " << clientSeq << " sendTime=" << reqState.sendTime
                      << " now=" << now << " due to timeout";

            reqState.triggerSent = true;
            reqState.triggerSendTime = now;
            lastSlowPath_ = clientSeq;

            FallbackTrigger fallbackTriggerMsg;

            fallbackTriggerMsg.set_client_id(clientId_);
            fallbackTriggerMsg.set_instance(myInstance_);
            fallbackTriggerMsg.set_client_seq(clientSeq);

            // TODO set request data
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }
        }
    }
}

void Client::updateInstance()
{
    uint32_t newInstance = myInstance_ - 1;
    int count = 0;
    do {
        newInstance++;
        count = 0;
        for (const auto &[rid, rinst] : replicaInstances_) {
            if (rinst > newInstance)
                count++;
        }
    } while (count >= f_ + 1);

    uint64_t now = GetMicrosecondTimestamp();
    if (newInstance != myInstance_) {
        VLOG(1) << "Updating instance=" << newInstance << " from " << myInstance_;

        // TODO Reset any uncommitted requests by changing the send time
        // for (auto &[cseq, reqState] : requestStates_) {
        //     // reqState.sendTime = now;
        //     // reqState.triggerSent = false;
        // }

        myInstance_ = newInstance;
    }
}

void Client::handleMessage(MessageHeader *hdr, byte *body, Address *sender)
{
    if (hdr->msgLen < 0) {
        return;
    }

    if (hdr->msgType == MessageType::REPLY || hdr->msgType == MessageType::FAST_REPLY) {
        Reply reply;

        // TODO verify and handle signed header better
        if (!reply.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse REPLY message";
            return;
        }

        if (reply.client_id() != clientId_) {
            VLOG(2) << "Received reply for client " << reply.client_id() << " != " << clientId_;
            return;
        }

        if (!sigProvider_.verify(hdr, body, "replica", reply.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        handleReply(reply, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    else if (hdr->msgType == MessageType::CERT_REPLY) {
        CertReply certReply;

        if (!certReply.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CERT_REPLY message";
            return;
        }

        if (certReply.client_id() != clientId_) {
            VLOG(2) << "Received certReply for client " << certReply.client_id() << " != " << clientId_;
            return;
        }

        if (!sigProvider_.verify(hdr, body, "replica", certReply.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature for CERT_REPLY!";
            return;
        }

        handleCertReply(certReply, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    else if (hdr->msgType == MessageType::FALLBACK_SUMMARY) {
        FallbackSummary fallbackSummary;

        if (!fallbackSummary.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse FALLBACK_SUMMARY message";
            return;
        }

        if (!sigProvider_.verify(hdr, body, "replica", fallbackSummary.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature for FALLBACK_SUMMARY!";
            return;
        }

        handleFallbackSummary(fallbackSummary, std::span{body + hdr->msgLen, hdr->sigLen});
    }
}

void Client::handleReply(dombft::proto::Reply &reply, std::span<byte> sig)
{
    uint32_t clientSeq = reply.client_seq();
    uint64_t now = GetMicrosecondTimestamp();

    // Update client instance
    replicaInstances_[reply.replica_id()] = std::max(reply.instance(), replicaInstances_[reply.replica_id()]);
    updateInstance();

    // Check validity
    if (requestStates_.count(clientSeq) == 0) {
        VLOG(2) << "Received reply for " << clientSeq << " not in active requests";
        return;
    }

    auto &reqState = requestStates_.at(clientSeq);

    VLOG(4) << "Received reply from replica " << reply.replica_id() << " instance " << reply.instance() << " for c_seq "
            << clientSeq << " at log pos " << reply.seq() << " after " << now - reqState.sendTime << " usec";

    bool hasCertBefore = reqState.collector.hasCert();
    uint32_t maxMatchSize = reqState.collector.insertReply(reply, std::vector<byte>(sig.begin(), sig.end()));

    // Just collected cert
    if (!hasCertBefore && reqState.collector.hasCert()) {
        VLOG(1) << "Created cert for request number " << clientSeq;
        reqState.certTime = now;
        return;
    }

    if (maxMatchSize == 3 * f_ + 1) {
        // TODO Deliver to application
        // Request is committed and can be cleaned up.
        VLOG(1) << "PERF event=commit path=fast" << " client_id=" << clientId_ << " client_seq=" << clientSeq
                << " seq=" << reply.seq() << " instance=" << reply.instance() << " latency=" << now - reqState.sendTime
                << " digest=" << digest_to_hex(reply.digest()).substr(56);

        lastFastPath_ = clientSeq;

        commitRequest(clientSeq);
        return;
    }

    // If the number of potential remaining replies is not enough to reach 2f + 1 for any matching reply,
    // we have a proof of inconsistency.
    if (!reqState.triggerSent && 3 * f_ + 1 - reqState.collector.replies_.size() < 2 * f_ + 1 - maxMatchSize) {
        LOG(INFO) << "Client detected cert is impossible, triggering fallback with proof for cseq=" << clientSeq;

        reqState.triggerSendTime = now;
        reqState.triggerSent = true;
        lastSlowPath_ = clientSeq;

        reqState.fallbackProof = Cert();
        FallbackTrigger fallbackTriggerMsg;

        fallbackTriggerMsg.set_client_id(clientId_);
        fallbackTriggerMsg.set_instance(myInstance_);
        fallbackTriggerMsg.set_client_seq(clientSeq);

        // TODO check if fast path is not posssible, and we can send cert right away
        for (auto &[replicaId, reply] : reqState.collector.replies_) {
            auto &sig = reqState.collector.signatures_[replicaId];
            reqState.fallbackProof->add_signatures(std::string(sig.begin(), sig.end()));
            (*reqState.fallbackProof->add_replies()) = reply;
        }

        // I think this is right, or we could do set_allocated_foo if fallbackProof was dynamically allcoated.
        (*fallbackTriggerMsg.mutable_proof()) = *reqState.fallbackProof;

        reqState.sendTime = GetMicrosecondTimestamp();
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }
}

void Client::handleCertReply(const CertReply &certReply, std::span<byte> sig)
{
    uint32_t cseq = certReply.client_seq();

    if (requestStates_.count(cseq) == 0) {
        // VLOG(2) << "Received certReply for " << cseq << " not in active requests";
        return;
    }

    auto &reqState = requestStates_.at(cseq);
    reqState.certReplies.insert(certReply.replica_id());

    if (reqState.certReplies.size() >= 2 * f_ + 1) {
        VLOG(1) << "PERF event=commit path=normal " << " client_id=" << clientId_ << " client_seq=" << cseq
                << " seq=" << certReply.seq() << " instance=" << certReply.instance()
                << " latency=" << GetMicrosecondTimestamp() - reqState.sendTime
                << " digest=" << digest_to_hex(reqState.collector.cert_->replies()[0].digest()).substr(56);
        lastNormalPath_ = cseq;
        commitRequest(cseq);
    }
}

void Client::handleFallbackSummary(const dombft::proto::FallbackSummary &summary, std::span<byte> sig)
{
    VLOG(2) << "Received fallback summary for instance=" << summary.instance()
            << " from replicaId=" << summary.replica_id();

    replicaInstances_[summary.replica_id()] = std::max(summary.instance(), replicaInstances_[summary.replica_id()]);
    updateInstance();

    bool slowPathCommitted = false;

    for (const FallbackReply &reply : summary.replies()) {
        if (reply.client_id() != clientId_)
            continue;

        uint32_t cseq = reply.client_seq();

        if (requestStates_.count(cseq) == 0)
            continue;

        auto &reqState = requestStates_.at(cseq);
        VLOG(1) << "Received fallback reply for c_seq=" << cseq;

        reqState.fallbackReplies.insert(summary.replica_id());
        if (reqState.fallbackReplies.size() >= 2 * f_ + 1) {
            // Request is committed, so we can clean up state!
            // TODO check we have a consistent set of application replies!

            VLOG(1) << "PERF event=commit path=slow client_id=" << clientId_ << " client_seq=" << cseq
                    << " seq=" << reply.seq() << "instance=" << summary.instance()
                    << " latency=" << GetMicrosecondTimestamp() - reqState.sendTime;

            lastSlowPath_ = cseq;
            commitRequest(cseq);
            slowPathCommitted = true;
        }
    }

    if (slowPathCommitted) {
        // If we committed anything in the slow path, retry any outstanding requests in the new instance
        for (auto &[cseq, reqState] : requestStates_) {
            reqState.request.set_instance(myInstance_);

            sendRequest(reqState.request);
            VLOG(1) << "Retrying cseq=" << reqState.client_seq << " after slow path commits!";
        }
    }
}

}   // namespace dombft
