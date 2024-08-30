#include "client.h"

#include "lib/transport/nng_endpoint.h"
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

    LOG(INFO) << "Sending " << config.clientNumRequests << " requests for at most " << config.clientRuntimeSeconds
              << " seconds";
    numRequests_ = config.clientNumRequests;

    LOG(INFO) << "Sending at most " << config.clientMaxRequests << " requests at once";

    maxInFlight_ = config.clientMaxRequests;
    clientSendRate_ = config.clientSendRate;

    if (config.clientSendMode == "sendRate") {
        sendMode_ = dombft::RateBased;
    } else if (config.clientSendMode == "maxRequests") {
        LOG(INFO) << "Send rate: " << clientSendRate_;
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

        endpoint_ = std::make_unique<NngEndpoint>(addrPairs, true);

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
    nextReqSeq_ = 1;

    MessageHandlerFunc replyHandler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->receiveReply(msgHdr, msgBuffer, sender);
    };

    endpoint_->RegisterMsgHandler(replyHandler);

    timeoutTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Client *) ctx)->checkTimeouts(); }, 5000, this);

    endpoint_->RegisterTimer(timeoutTimer_.get());

    if (config.clientBackPressureMode == "none") {
        backpressureMode_ = dombft::None;
    } else if (config.clientBackPressureMode == "sleep") {
        backpressureMode_ = dombft::Sleep;
        clientBackPressureSleepTime = config.clientBackPressureSleepTime;
        LOG(INFO) << "the backpressure recovery time is " << clientBackPressureSleepTime << " seconds";
    } else if (config.clientBackPressureMode == "adjust") {
        backpressureMode_ = dombft::Adjust;
    }

    if (sendMode_ == dombft::RateBased) {
        sendTimer_ = std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Client *) ctx)->submitRequest(); },
                                             1000000 / clientSendRate_, this);

        endpoint_->RegisterTimer(sendTimer_.get());
    }

    terminateTimer_ = std::make_unique<Timer>(
        [config](void *ctx, void *endpoint) {
            LOG(INFO) << "Exiting  after running for " << config.clientRuntimeSeconds << " seconds";
            // TODO print some stats
            exit(0);
        },
        config.clientRuntimeSeconds * 1000000,   // timer is in us.
        this);

    endpoint_->RegisterTimer(terminateTimer_.get());

    if (config.app == AppType::COUNTER) {
        trafficGen_ = std::make_unique<CounterTrafficGen>();
        appType_ = AppType::COUNTER;
    } else {
        LOG(ERROR) << "Unknown application type for client!";
        exit(1);
    }

    LOG(INFO) << "Client finished initializing";
}

Client::~Client()
{
    // TODO cleanup... though we don't really reuse this
}

void Client::run()
{
    if (sendMode_ == dombft::MaxInFlightBased) {
        for (uint32_t i = 0; i < maxInFlight_; i++) {
            submitRequest();
        }
    }
    endpoint_->LoopRun();
}

void Client::receiveReply(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    if (msgHdr->msgLen < 0) {
        return;
    }

    if (msgHdr->msgType == MessageType::REPLY || msgHdr->msgType == MessageType::FAST_REPLY) {
        Reply reply;

        // TODO verify and handle signed header better
        if (!reply.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse REPLY message";
            return;
        }

        if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", reply.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        // Check validity
        //  1. Not for the same client
        //  2. Client doesn't have state for this request
        if (reply.client_id() != clientId_) {
            // TODO more info?
            LOG(INFO) << "Invalid reply! " << reply.client_id() << ", " << reply.client_seq();
            return;
        }

        if (requestStates_.count(reply.client_seq()) == 0) {
            VLOG(2) << "Received reply for " << reply.client_seq() << " not in active requests";
            return;
        }

        auto &reqState = requestStates_[reply.client_seq()];

        VLOG(4) << "Received reply from replica " << reply.replica_id() << " instance " << reply.instance() << " for "
                << reply.client_seq() << " at log pos " << reply.seq() << " after "
                << GetMicrosecondTimestamp() - reqState.sendTime << " usec";

        // TODO handle dups
        reqState.replies[reply.replica_id()] = reply;
        reqState.signatures[reply.replica_id()] = sigProvider_.getSignature(msgHdr, msgBuffer);
        reqState.instance = std::max(reqState.instance, reply.instance());

#if PROTOCOL == PBFT
        if (numReplies_[reply.client_seq()] >= f_ / 3 * 2 + 1) {

            LOG(INFO) << "PBFT commit for " << reply.client_seq() - 1 << " took "
                      << GetMicrosecondTimestamp() - sendTimes_[reply.client_seq()] << " usec";

            numReplies_.erase(reply.client_seq());
            sendTimes_.erase(reply.client_seq());

            numExecuted_++;
            if (numExecuted_ >= 1000) {
                exit(0);
            }

            if (sendMode_ == dombft::maxInFlightBased) {
                submitRequest();
            }
        }

#else
        checkReqState(reply.client_seq());
#endif
    }

    else if (msgHdr->msgType == MessageType::CERT_REPLY) {
        CertReply certReply;

        if (!certReply.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CERT_REPLY message";
            return;
        }

        uint32_t cseq = certReply.client_seq();

        if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", certReply.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature for CERT_REPLY!";
            return;
        }

        if (certReply.client_id() != clientId_) {
            VLOG(2) << "Received certReply for client " << certReply.client_id() << " != " << clientId_;
            return;
        }

        if (requestStates_.count(cseq) == 0) {
            // VLOG(2) << "Received certReply for " << cseq << " not in active requests";
            return;
        }

        auto &reqState = requestStates_[cseq];

        reqState.certReplies.insert(certReply.replica_id());

        if (reqState.certReplies.size() >= 2 * f_ + 1) {
            // Request is committed, so we can clean up state!
            // TODO check we have a consistent set of application replies!

            VLOG(1) << "Request " << cseq << " normal path committed! "
                    << "Took " << GetMicrosecondTimestamp() - requestStates_[cseq].sendTime << " us";

            requestStates_.erase(cseq);
            numCommitted_++;
            inFlight_--;

            if (sendMode_ == dombft::MaxInFlightBased) {
                submitRequest();
            }
        }
    }

    if (numCommitted_ >= numRequests_) {
        LOG(INFO) << "Exiting before after committing " << numRequests_ << " requests";
        exit(0);
    }
}

void Client::submitRequest()
{
    LOG(INFO) << "Inflight txns (before submitting)  " << inFlight_;
    if (inFlight_ > maxInFlight_) {
        adjustSendRate();
    }
    ClientRequest request;

    // submit new request
    request.set_client_id(clientId_);
    request.set_client_seq(nextReqSeq_);
    request.set_send_time(GetMicrosecondTimestamp());
    request.set_is_write(true);   // TODO modify this based on some random chance

    auto appRequest = trafficGen_->generateAppTraffic();
    // TODO: this has to be hard coded in an inelegant way. May imporve this later
    if (appType_ == AppType::COUNTER) {
        dombft::apps::CounterRequest *counterReq = (dombft::apps::CounterRequest *) appRequest;
        request.set_req_data(counterReq->SerializeAsString());
    }

    requestStates_[nextReqSeq_].sendTime = request.send_time();

#if USE_PROXY && PROTOCOL == DOMBFT
    // TODO how to choose proxy, perhaps by IP or config
    // VLOG(4) << "Begin sending request number " << nextReqSeq_;
    Address &addr = proxyAddrs_[clientId_ % proxyAddrs_.size()];
    // TODO maybe client should own the memory instead of endpoint.
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
    // VLOG(4) << "Serialization Done " << nextReqSeq_;
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    // VLOG(4) << "Signature Done " << nextReqSeq_;

    endpoint_->SendPreparedMsgTo(addr);
    VLOG(1) << "Sent request number " << nextReqSeq_ << " to " << addr.GetIPAsString();

#else
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
    // TODO check errors for all of these lol
    // TODO do this while waiting, not in the critical path
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr);
    }
#endif

    nextReqSeq_++;
    inFlight_++;

    LOG(INFO) << "Inflight txns  " << inFlight_;

    if (inFlight_ > maxInFlight_) {
        adjustSendRate();
    }
}

void Client::adjustSendRate()
{
    if (sendMode_ == dombft::MaxInFlightBased) {
        // this mode does not adjusr send rate
        return;
    }

    if (backpressureMode_ == dombft::None) {
        LOG(INFO) << "backpressure mode is none, does not adjust";
        return;
    }

    if (backpressureMode_ == dombft::Adjust) {
        LOG(WARNING) << "backpressure mode adjust not yet implemented";
        return;
    }

    if (backpressureMode_ == dombft::Sleep) {
        LOG(INFO) << "backpressure mode sleep triggered";
        endpoint_->PauseTimer(sendTimer_.get(), clientBackPressureSleepTime);

        LOG(INFO) << "adjust timer called";
        return;
    }
}

void Client::checkTimeouts()
{
    uint64_t now = GetMicrosecondTimestamp();

    for (auto &entry : requestStates_) {
        int clientSeq = entry.first;
        RequestState &reqState = entry.second;

        if (reqState.cert.has_value() && now - reqState.certTime > normalPathTimeout_) {
            VLOG(1) << "Request number " << clientSeq << " fast path timed out! Sending cert!";

            // Send cert to replicas;
            endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }

            VLOG(1) << "Cert sent!";
            reqState.certTime = now;   // timeout again later
        }

        if (now - reqState.sendTime > slowPathTimeout_) {
            LOG(INFO) << "Client attempting fallback on request " << clientSeq << " sendTime=" << reqState.sendTime
                      << " now=" << now << " due to timeout";
            reqState.sendTime = now;

            reqState.fallbackAttempts++;

            if (reqState.fallbackAttempts == 10) {
                LOG(ERROR) << "Client failed on request " << clientSeq << " after 10 attempts";
                exit(1);
            }

            FallbackTrigger fallbackTriggerMsg;

            fallbackTriggerMsg.set_client_id(clientId_);
            fallbackTriggerMsg.set_instance(reqState.instance);
            fallbackTriggerMsg.set_client_seq(clientSeq);

            if (reqState.fallbackProof.has_value()) {
                (*fallbackTriggerMsg.mutable_proof()) = *reqState.fallbackProof;
            }

            // TODO set request data
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }

            LOG(ERROR) << "Attempting slow path again in " << slowPathTimeout_ << " usec";
        }
    }
}

void Client::checkReqState(uint32_t clientSeq)
{
    auto &reqState = requestStates_[clientSeq];

    if (reqState.cert.has_value() && reqState.replies.size() == 3 * f_ + 1) {
        // We already have a cert, so fast path might be possible.
        // TODO check if fast path is not posssible and prevent extra work
        // TODO this fast path is quite basic, since it just checks for 3 f + 1 fast replies
        // we want it to do a bit more, specifically
        // 2. Send certs to replicas with normal path to catch them up

        auto it = reqState.replies.begin();
        std::optional<std::string> result;

        const Reply &rep1 = it->second;
        if (!rep1.fast()) {
            // This means all responses need to be fast
            return;
        }

        it++;
        for (; it != reqState.replies.end(); it++) {
            const Reply &rep2 = it->second;
            if (rep1.seq() != rep2.seq() || rep1.digest() != rep2.digest() || rep1.result() != rep2.result())
                return;
        }

        // TODO Deliver to application
        // Request is committed and can be cleaned up.

        VLOG(1) << "Request " << rep1.client_seq() << " fast path committed at global seq " << rep1.seq() << ". Took "
                << GetMicrosecondTimestamp() - requestStates_[rep1.client_seq()].sendTime << " us";

        requestStates_.erase(clientSeq);
        numCommitted_++;
        inFlight_--;

        if (sendMode_ == dombft::MaxInFlightBased) {
            submitRequest();
        }
    } else {

        // Try and find a certificate or proof of divergent histories
        std::map<std::tuple<std::string, int>, std::set<int>> matchingReplies;
        size_t maxMatching = 0;

        for (const auto &entry : reqState.replies) {
            int replicaId = entry.first;
            const Reply &reply = entry.second;

            // TODO this is ugly lol
            // We don't need client_seq/client_id, these are already checked.
            // We also don't check the result here, that only needs to happen in the fast path
            std::tuple<std::string, int> key = {reply.digest(), reply.seq()};

            VLOG(4) << digest_to_hex(reply.digest()).substr(56) << " " << reply.seq() << " " << reply.instance();

            matchingReplies[key].insert(replicaId);
            maxMatching = std::max(maxMatching, matchingReplies[key].size());

            if (matchingReplies[key].size() >= 2 * f_ + 1) {
                reqState.cert = Cert();
                reqState.cert->set_seq(std::get<1>(key));

                // TODO check if fast path is not posssible, and we can send cert right away
                for (auto repId : matchingReplies[key]) {
                    reqState.cert->add_signatures(reqState.signatures[repId]);
                    // THis usage is so weird, is protobuf the right tool?
                    (*reqState.cert->add_replies()) = reqState.replies[repId];
                }

                reqState.certTime = GetMicrosecondTimestamp();

                VLOG(1) << "Created cert for request number " << clientSeq;

                return;
            }
        }

        // If the number of potential remaining replies is not enough to reach 2f + 1 for any matching reply,
        // we have a proof of inconsistency.
        if (reqState.fallbackAttempts == 0 && 3 * f_ + 1 - reqState.replies.size() < 2 * f_ + 1 - maxMatching) {

            // TODO check instances better
            std::set<int> instances;
            for (auto &replyEntry : reqState.replies) {
                instances.insert(replyEntry.second.instance());
            }
            if (instances.size() > 1) {
                LOG(INFO) << "Skipping proof due to multiple instances";
                return;
            }

            LOG(INFO) << "Client detected cert is impossible, triggering fallback with proof for cseq=" << clientSeq;

            reqState.fallbackProof = Cert();
            FallbackTrigger fallbackTriggerMsg;

            fallbackTriggerMsg.set_client_id(clientId_);
            fallbackTriggerMsg.set_instance(reqState.instance);
            fallbackTriggerMsg.set_client_seq(clientSeq);

            // TODO check if fast path is not posssible, and we can send cert right away
            for (auto &replyEntry : reqState.replies) {
                reqState.fallbackProof->add_signatures(reqState.signatures[replyEntry.first]);
                (*reqState.fallbackProof->add_replies()) = replyEntry.second;
            }

            // I think this is right, or we could do set_allocated_foo if fallbackProof was dynamically allcoated.
            (*fallbackTriggerMsg.mutable_proof()) = *reqState.fallbackProof;

            reqState.fallbackAttempts++;

            reqState.sendTime = GetMicrosecondTimestamp();
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackTriggerMsg, FALLBACK_TRIGGER);
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }
        }
    }
}

}   // namespace dombft