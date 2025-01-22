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
    , threadpool_(4)
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
    requestSize_ = config.clientRequestSize;

    if (config.clientSendMode == "sendRate") {
        sendMode_ = dombft::RateBased;
        LOG(INFO) << "Send rate: " << sendRate_;
    } else if (config.clientSendMode == "maxInFlight") {
        sendMode_ = dombft::MaxInFlightBased;
    }

    /* Setup keys */
    std::string clientKey = config.clientKeysDir + "/client" + std::to_string(clientId_) + ".der";
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
    startTime_ = GetMicrosecondTimestamp();

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
        this
    );

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
        // Kick off sending with a small burst every 5 ms
        lastSendTime_ = GetMicrosecondTimestamp();
        sendTimer_ = std::make_unique<Timer>([&](void *ctx, void *endpoint) { submitRequestsOpenLoop(); }, 5000, this);
        endpoint_->RegisterTimer(sendTimer_.get());

    } else if (sendMode_ == dombft::MaxInFlightBased) {
        for (uint32_t i = 0; i < maxInFlight_; i++) {
            submitRequest();
        }
    } else {
        LOG(ERROR) << "Unknown send mode type for client!";
        exit(1);
    }

    MessageHandlerFunc replyHandler =
        [this, runtime = config.clientRuntimeSeconds](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
            if (GetMicrosecondTimestamp() - startTime_ > 1000000 * runtime) {
                LOG(INFO) << "Exiting  after running for " << runtime << " seconds through message handler";
                // TODO print some stats
                exit(0);
            }

            this->handleMessage(msgHdr, msgBuffer, sender);

            if (sendMode_ == dombft::RateBased) {
                submitRequestsOpenLoop();
            }
        };

    endpoint_->RegisterMsgHandler(replyHandler);

    // Handle interrupt signals properly on main loop
    endpoint_->RegisterSignalHandler([&]() { endpoint_->LoopBreak(); });

    LOG(INFO) << "Client main thread starting";
    endpoint_->LoopRun();
    LOG(INFO) << "Client main thread finished";
}

Client::~Client()
{
    // TODO cleanup... though we don't really reuse this
}

void Client::fillRequestData(ClientRequest &request)
{
    auto appRequest = trafficGen_->generateAppTraffic();
    // TODO: this has to be hard coded in an inelegant way. May imporve this later
    std::string reqData;

    if (appType_ == AppType::COUNTER) {
        dombft::apps::CounterRequest *counterReq = (dombft::apps::CounterRequest *) appRequest;
        reqData = counterReq->SerializeAsString();
    }

    if (reqData.size() < requestSize_) {
        request.set_padding(std::string(requestSize_ - reqData.size(), '\0'));
    }

    request.set_req_data(reqData);
}

void Client::submitRequest()
{
    ClientRequest request;

    uint64_t now = GetMicrosecondTimestamp();

    // submit new request
    request.set_client_id(clientId_);
    request.set_client_seq(nextSeq_);
    request.set_send_time(now);
    request.set_is_write(true);   // TODO modify this based on some random chance

    fillRequestData(request);

    requestStates_.emplace(nextSeq_, RequestState(f_, request, now));

    threadpool_.enqueueTask([=, this](byte *buffer) { sendRequest(request, buffer); });

    VLOG(1) << "PERF event=send" << " client_id=" << clientId_ << " client_seq=" << nextSeq_
            << " in_flight=" << numInFlight_;

    nextSeq_++;
    numInFlight_++;
}

void Client::submitRequestsOpenLoop()
{
    // If we are in the slow path, don't submit anymore
    if (std::max(lastFastPath_, lastNormalPath_) < lastSlowPath_ && numInFlight_ >= 1) {
        VLOG(6) << "Pause sending because slow path: lastFastPath_=" << lastFastPath_
                << " lastNormalPath_=" << lastNormalPath_ << " lastSlowPath_=" << lastSlowPath_
                << " numInFlight=" << numInFlight_;

        return;
    }

    uint64_t startSendTime = GetMicrosecondTimestamp();
    uint64_t actualSendRate = lastFastPath_ < lastNormalPath_ ? sendRate_ / replicaAddrs_.size() : sendRate_;
    double sendIntervalUs = 1000000.0 / actualSendRate;

    uint64_t numToSend = (startSendTime - lastSendTime_) * actualSendRate / 1000000.0;

    if (numToSend == 0) {
        return;
    }

    VLOG(5) << "Sending burst of " << numToSend << " requests after " << startSendTime - lastSendTime_
            << " us since last burst with send interval " << sendIntervalUs << "us";

    // Rather than just setting lastSendTime here, add the number of requests sent * sendInterval, so
    // that we account for rounding errors.
    lastSendTime_ += numToSend * sendIntervalUs;

    std::vector<ClientRequest> requests;
    uint64_t now;
    for (uint32_t i = 0; i < numToSend; i++) {
        now = GetMicrosecondTimestamp();

        if (numInFlight_ >= maxInFlight_) {
            VLOG(5) << "Only send " << i << " requests in burst because maxInFlight_=" << maxInFlight_ << " reached";
            break;
        }

        ClientRequest &request = requests.emplace_back();

        // submit new request
        request.set_client_id(clientId_);
        request.set_client_seq(nextSeq_);
        request.set_send_time(now);
        request.set_is_write(true);   // TODO modify this based on some random chance

        fillRequestData(request);

        requestStates_.emplace(nextSeq_, RequestState(f_, request, now));
        VLOG(1) << "PERF event=send" << " client_id=" << clientId_ << " client_seq=" << nextSeq_
                << " in_flight=" << numInFlight_;

        nextSeq_++;
        numInFlight_++;
    }

    threadpool_.enqueueTask([=, this](byte *buffer) {
        for (const ClientRequest &req : requests) {
            sendRequest(req, buffer);
        }
    });
}

void Client::retryRequests()
{
    for (auto &[cseq, reqState] : requestStates_) {
        uint64_t now = GetMicrosecondTimestamp();
        ClientRequest &req = reqState.request;
        req.set_send_time(now);
        reqState = RequestState(f_, req, now);
        threadpool_.enqueueTask([=, this](byte *buffer) { sendRequest(req, buffer); });
        VLOG(1) << "Retrying cseq=" << reqState.client_seq << " after instance/view update";
    }
}

void Client::sendRequest(const ClientRequest &request, byte *buffer)
{
#if USE_PROXY
    // TODO how to choose proxy, perhaps by IP or config
    // VLOG(4) << "Begin sending request number " << nextReqSeq_;
    Address &addr = proxyAddrs_[clientId_ % proxyAddrs_.size()];
    // TODO maybe client should own the memory instead of endpoint.
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST, buffer);
    // VLOG(4) << "Serialization Done " << nextReqSeq_;
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    // VLOG(4) << "Signature Done " << nextReqSeq_;

    endpoint_->SendPreparedMsgTo(addr, hdr);
#else
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST, buffer);
    // TODO check errors for all of these lol
    // TODO do this while waiting, not in the critical path
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

#if SEND_TO_LEADER
    VLOG(1) << "Sending request directly to " << replicaAddrs_[0];

    endpoint_->SendPreparedMsgTo(replicaAddrs_[0], hdr);
#else
    VLOG(1) << "Sending request to all replicas " << replicaAddrs_[0];
    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr, hdr);
    }
#endif
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
            fallbackTriggerMsg.set_client_seq(clientSeq);

            // TODO set request data
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }
        }

        if (reqState.triggerSent && now - reqState.triggerSendTime > slowPathTimeout_) {
            // This is expected to happen when the replicas are making progress
            LOG(INFO) << "Client fallback on request " << clientSeq << " timed out again, retrying request through DOM";
            ClientRequest &req = reqState.request;
            req.set_send_time(now);

            reqState = RequestState(f_, req, now);
            threadpool_.enqueueTask([=, this](byte *buffer) { sendRequest(req, buffer); });
        }
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

        if (!sigProvider_.verify(hdr, "replica", reply.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature for reply! replica_id=" << reply.replica_id();
            return;
        }

        handleReply(reply, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    else if (hdr->msgType == MessageType::CERT_REPLY) {
        CertReply certReply;

        if (!certReply.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CERT_REPLY message from " << *sender;
            return;
        }

        if (certReply.client_id() != clientId_) {
            VLOG(2) << "Received certReply for client " << certReply.client_id() << " != " << clientId_ << " from "
                    << certReply.replica_id();
            return;
        }

        if (!sigProvider_.verify(hdr, "replica", certReply.replica_id())) {
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

        if (!sigProvider_.verify(hdr, "replica", fallbackSummary.replica_id())) {
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
        VLOG(2) << "Created cert for request number " << clientSeq;
        reqState.certTime = now;
    }
    if (maxMatchSize == 3 * f_ + 1) {
        // TODO Deliver to application
        // Request is committed and can be cleaned up.
        std::string seq = reply.retry() ? "UNKNOWN" : std::to_string(reply.seq());
        VLOG(1) << "PERF event=commit path=fast" << " client_id=" << clientId_ << " client_seq=" << clientSeq
                << " seq=" << seq << " instance=" << reply.instance() << " latency=" << now - reqState.sendTime
                << " digest=" << digest_to_hex(reply.digest()).substr(56);

        lastFastPath_ = clientSeq;

        commitRequest(clientSeq);
        return;
    }

    // `replies_.size() == maxMatchSize` iff all replies are yet matching, no need to check for normal/slow path
    // return when normal/slow path is already triggered
    if (reqState.collector.replies_.size() == maxMatchSize || reqState.certSent || reqState.triggerSent)
        return;

    // `hasCert()==true` iff maxMatchSize >= 2 * f_ + 1
    if (reqState.collector.hasCert()) {
        LOG(INFO) << "Request number " << clientSeq << " fast path impossible, has cert. Sending cert!";
        reqState.certSent = true;

        lastNormalPath_ = clientSeq;

        // Send cert to replicas
        endpoint_->PrepareProtoMsg(reqState.collector.getCert(), CERT);
        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }

    // If the number of potential remaining replies is not enough to reach 2f + 1 for any matching reply,
    // we have a proof of inconsistency.
    if (reqState.collector.replies_.size() - maxMatchSize > f_) {
        LOG(INFO) << "Client detected cert is impossible, triggering fallback with proof for cseq=" << clientSeq;

        reqState.triggerSendTime = now;
        reqState.triggerSent = true;
        lastSlowPath_ = clientSeq;

        reqState.fallbackProof = Cert();
        FallbackTrigger fallbackTriggerMsg;

        fallbackTriggerMsg.set_client_id(clientId_);
        fallbackTriggerMsg.set_client_seq(clientSeq);

        for (auto &[replicaId, reply] : reqState.collector.replies_) {
            auto &sig = reqState.collector.signatures_[replicaId];
            reqState.fallbackProof->add_signatures(std::string(sig.begin(), sig.end()));
            (*reqState.fallbackProof->add_replies()) = reply;
        }

        // I think this is right, or we could do set_allocated_foo if fallbackProof was dynamically allcoated.
        (*fallbackTriggerMsg.mutable_proof()) = *reqState.fallbackProof;

        reqState.sendTime = GetMicrosecondTimestamp();
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
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

    VLOG(4) << "Received cert ack client_seq=" << cseq << " seq=" << certReply.seq()
            << " instance=" << certReply.instance() << " replica_id=" << certReply.replica_id();

    if (reqState.certReplies.size() >= 2 * f_ + 1) {
        VLOG(1) << "PERF event=commit path=normal client_id=" << clientId_ << " client_seq=" << cseq
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

    for (const FallbackReply &reply : summary.replies()) {
        if (reply.client_id() != clientId_)
            continue;

        uint32_t cseq = reply.client_seq();

        if (requestStates_.count(cseq) == 0)
            continue;

        auto &reqState = requestStates_.at(cseq);

        reqState.fallbackReplies.insert(summary.replica_id());
        if (reqState.fallbackReplies.size() >= f_ + 1) {
            // Request is committed, so we can clean up state!
            // TODO check we have a consistent set of application replies!

            VLOG(1) << "PERF event=commit path=slow client_id=" << clientId_ << " client_seq=" << cseq
                    << " seq=" << reply.seq() << " instance=" << summary.instance()
                    << " latency=" << GetMicrosecondTimestamp() - reqState.sendTime;

            lastSlowPath_ = cseq;
            commitRequest(cseq);
        }
    }
}

}   // namespace dombft
