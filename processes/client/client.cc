#include "client.h"

#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/apps/kv_store.h"
#include "lib/client_record.h"
#include "proto/dombft_apps.pb.h"

#define NUM_CLIENTS 100

namespace dombft {
using namespace dombft::proto;

Client::Client(size_t id)
    : clientId_(id)
    , threadpool_(4)
{
    LOG(INFO) << "clientId=" << clientId_;

    auto &configManager = ConfigManager::getInstance();
    const auto &config = configManager.getConfig();
    const auto &clientIps = configManager.getClientIps();

    std::string clientIp = clientIps[clientId_];
    LOG(INFO) << "clientIp=" << clientIp;
    int clientPort = configManager.getClientPort();
    LOG(INFO) << "clientPort=" << clientPort;

    // Use ConfigManager's pre-calculated BFT parameters
    f_ = config.f;
    quorumSize_ = configManager.getQuorumSize();
    superQuorumSize_ = configManager.getSuperQuorumSize();

    normalPathEnabled_ = config.clientNormalPathEnabled;

    normalPathTimeout_ = config.clientNormalPathTimeout;
    requestTimeout_ = config.clientRequestTimeout;

    LOG(INFO) << "Running for " << config.clientRuntimeSeconds << " seconds";

    LOG(INFO) << "Sending at most " << config.clientMaxInFlight << " requests at once";

    maxInFlight_ = config.clientMaxInFlight;
    sendRate_ = config.clientSendRate;
    requestSize_ = config.clientRequestSize;
    useHMAC_ = config.clientUseHMAC;

    // Load temporary rate increase configuration
    temporaryRateIncreaseEnabled_ = config.clientTemporaryRateIncrease.enabled;
    rateIncreaseSeqThreshold_ = config.clientTemporaryRateIncrease.seqThreshold;
    rateIncreaseDurationUs_ = config.clientTemporaryRateIncrease.durationUs;
    increasedSendRate_ = config.clientTemporaryRateIncrease.increasedSendRate;
    increasedMaxInFlight_ = config.clientTemporaryRateIncrease.increasedMaxInFlight;

    if (temporaryRateIncreaseEnabled_) {
        LOG(INFO) << "Temporary rate increase enabled:";
        LOG(INFO) << "  Trigger at sequence number: " << rateIncreaseSeqThreshold_;
        LOG(INFO) << "  Duration: " << rateIncreaseDurationUs_ / 1000000.0 << " seconds";
        LOG(INFO) << "  Increased send rate: " << increasedSendRate_;
        LOG(INFO) << "  Increased max in flight: " << increasedMaxInFlight_;
    }

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

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Error loading replica public keys, exiting...";
        exit(1);
    }

    hmacProvider_.loadClientKeysDev({NodeType::CLIENT, clientId_}, configManager.getNumReplicas());

    /** Setup transport */
    if (config.transport == "nng") {
        auto addrPairs = getClientAddrs(config, clientId_);

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true);

        size_t nReplicas = configManager.getNumReplicas();
        for (size_t i = 0; i < nReplicas; i++)
            replicaAddrs_.push_back(addrPairs[i].second);

        for (size_t i = nReplicas; i < addrPairs.size(); i++) {
            proxyAddrs_.push_back(addrPairs[i].second);
            VLOG(1) << proxyAddrs_.back();
        }

    } else if (config.transport == "simple-rpc") {
        /** Store all proxy addrs. */

        std::vector<Address> allAddrs;

        const auto &proxyIps = configManager.getProxyIps();
        for (uint32_t i = 0; i < proxyIps.size(); i++) {
            LOG(INFO) << "Proxy " << i + 1 << ": " << proxyIps[i] << ", " << configManager.getProxyForwardPort();
            proxyAddrs_.push_back(Address(proxyIps[i], configManager.getProxyForwardPort()));
            allAddrs.push_back(proxyAddrs_[i]);
        }

        /** Store all replica addrs */
        const auto &replicaIps = configManager.getReplicaIps();
        for (uint32_t i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
            allAddrs.push_back(replicaAddrs_[i]);
        }
        endpoint_ = std::make_unique<OOORPCEndpoint>(clientIp, clientPort + clientId_, allAddrs);

    } else {
        endpoint_ = std::make_unique<UDPEndpoint>(clientIp, clientPort, true);

        /** Store all proxy addrs. TODO handle mutliple proxy sockets*/
        const auto &proxyIps = configManager.getProxyIps();
        for (uint32_t i = 0; i < proxyIps.size(); i++) {
            LOG(INFO) << "Proxy " << i + 1 << ": " << proxyIps[i] << ", " << configManager.getProxyForwardPort();
            proxyAddrs_.push_back(Address(proxyIps[i], configManager.getProxyForwardPort()));
        }

        /** Store all replica addrs */
        const auto &replicaIps = configManager.getReplicaIps();
        for (uint32_t i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
        }
    }

    /** Initialize state */
    nextSeq_ = 1;
    startTime_ = GetMicrosecondTimestamp();

    timeoutTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Client *) ctx)->checkTimeouts(); }, 5000, this);

    endpoint_->RegisterTimer(timeoutTimer_.get());

    uint32_t runtimeSeconds = config.clientRuntimeSeconds;
    terminateTimer_ = std::make_unique<Timer>(
        [runtimeSeconds](void *ctx, void *endpoint) {
            LOG(INFO) << "Exiting after running for " << runtimeSeconds << " seconds";
            // TODO print some stats
            exit(0);
        },
        runtimeSeconds * 1000000,   // timer is in us.
        this
    );

    // Set high priority (lower is more priority) to terminate properly.
    ev_set_priority(terminateTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(terminateTimer_.get());

    if (config.app == AppType::COUNTER) {
        trafficGen_ = std::make_unique<CounterClient>();
        appType_ = AppType::COUNTER;
    } else if (config.app == AppType::KV_STORE) {
        trafficGen_ = std::make_unique<KVStoreClient>();
        appType_ = AppType::KV_STORE;
    } else {
        LOG(ERROR) << "Unknown application type for client!";
        exit(1);
    }

    MessageHandlerFunc replyHandler =
        [this, runtime = config.clientRuntimeSeconds](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
            if (GetMicrosecondTimestamp() - startTime_ > 1000000 * runtime) {
                LOG(INFO) << "Exiting after running for " << runtime << " seconds through message handler";
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

    if (sendMode_ == dombft::RateBased) {
        // Kick off sending with a small burst every 5 ms
        sendTimer_ = std::make_unique<Timer>([&](void *ctx, void *endpoint) { submitRequestsOpenLoop(); }, 5000, this);
        endpoint_->RegisterTimer(sendTimer_.get());

        endpoint_->Connect();

        // Send first request immediately, since we will wait it to be committed before sending more
        submitRequest();

    } else if (sendMode_ == dombft::MaxInFlightBased) {
        endpoint_->Connect();
        for (uint32_t i = 0; i < maxInFlight_; i++) {
            submitRequest();
        }
    } else {
        LOG(ERROR) << "Unknown send mode type for client!";
        exit(1);
    }

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
    std::string reqData = trafficGen_->generateAppRequest();

    PaddedRequestData data;

    data.set_req_data(reqData);

    if (reqData.size() < requestSize_) {
        data.set_padding(std::string(requestSize_ - reqData.size(), '\0'));
    }

    request.set_req_data(data.SerializeAsString());
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

    requestStates_.emplace(nextSeq_, RequestState(request, now));

    threadpool_.enqueueTask([=, this](byte *buffer) { sendRequest(request, buffer); });

    VLOG(1) << "PERF event=send" << " client_id=" << clientId_ << " client_seq=" << nextSeq_
            << " in_flight=" << numInFlight_;

    nextSeq_++;
    numInFlight_++;
}

void Client::submitRequestsOpenLoop()
{
    // Don't start rate-based sending until first request is committed
    if (!firstRequestCommitted_) {
        return;
    }

    uint64_t startSendTime = GetMicrosecondTimestamp();

    // Check if temporary rate increase should be deactivated
    if (rateIncreaseActive_ && (startSendTime - rateIncreaseStartTime_) >= rateIncreaseDurationUs_) {
        rateIncreaseActive_ = false;
        LOG(INFO) << "Temporary rate increase period ended after " << rateIncreaseDurationUs_ / 1000000.0 << " seconds";
        LOG(INFO) << "  Send rate: " << increasedSendRate_ << " -> " << sendRate_;
        LOG(INFO) << "  Max in flight: " << increasedMaxInFlight_ << " -> " << maxInFlight_;
    }

    // Use increased rate/max in flight if temporary increase is active
    uint32_t currentSendRate = rateIncreaseActive_ ? increasedSendRate_ : sendRate_;
    uint32_t currentMaxInFlight = rateIncreaseActive_ ? increasedMaxInFlight_ : maxInFlight_;

    double sendIntervalUs = 1000000.0 / currentSendRate;

    uint64_t numToSend = (startSendTime - lastSendTime_) * currentSendRate / 1000000.0;

    // VLOG(5) << "Sending burst of " << numToSend << " requests after " << startSendTime - lastSendTime_
    //         << " us since last burst with send interval " << sendIntervalUs << "us";

    if (numToSend == 0) {
        return;
    }

    // Rather than just setting lastSendTime at the end, add the number of requests sent * sendInterval, so
    // that we account for accumulating errors from sending
    lastSendTime_ += numToSend * sendIntervalUs;

    std::vector<ClientRequest> requests;
    uint64_t now;
    for (uint32_t i = 0; i < numToSend; i++) {
        now = GetMicrosecondTimestamp();

        if (numInFlight_ >= currentMaxInFlight) {
            // VLOG(5) << "Only send " << i << " requests in burst because maxInFlight_=" << currentMaxInFlight << "
            // reached";
            break;
        }

        ClientRequest &request = requests.emplace_back();

        // submit new request
        request.set_client_id(clientId_);
        request.set_client_seq(nextSeq_);
        request.set_send_time(now);
        request.set_is_write(true);   // TODO modify this based on some random chance

        fillRequestData(request);

        requestStates_.emplace(nextSeq_, RequestState(request, now));
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

void Client::sendRequest(const ClientRequest &request, byte *buffer)
{
#if USE_PROXY
    // TODO how to choose proxy, perhaps by IP or config
    Address &addr = proxyAddrs_[clientId_ % proxyAddrs_.size()];
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST, buffer);

    if (useHMAC_) {
        // TODO send multiple requests for each replica with their own hmacs
        hmacProvider_.appendMAC(hdr, SEND_BUFFER_SIZE, {NodeType::REPLICA, 0});
    } else {
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    }

    endpoint_->SendPreparedMsgTo(addr, hdr);
#else
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST, buffer);
    // TODO check errors for all of these lol
    // TODO do this while waiting, not in the critical path
    if (useHMAC_) {
        // TODO send multiple requests for each replica with their own hmacs
        hmacProvider_.appendMAC(hdr, SEND_BUFFER_SIZE, {NodeType::REPLICA, 0});
    } else {
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    }

#if SEND_TO_LEADER
    VLOG(1) << "Sending request directly to " << replicaAddrs_[0];

    endpoint_->SendPreparedMsgTo(replicaAddrs_[0], hdr);
#else
    VLOG(1) << "Sending request to all replicas ";
    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr, hdr);
    }
#endif
#endif
}

void Client::commitRequest(uint32_t clientSeq, uint64_t replicaSeq)
{
    // TODO inform application of result
    if (clientSeq > lastCommitted_ + 1) {
        LOG(WARNING) << "Committed out of order! Commited " << clientSeq << " after committing " << lastCommitted_;
    }
    lastCommitted_ = clientSeq;

    requestStates_.erase(clientSeq);
    numCommitted_++;
    numInFlight_--;

    // Enable rate-based sending after first request is committed
    if (!firstRequestCommitted_) {
        firstRequestCommitted_ = true;
        if (sendMode_ == dombft::RateBased) {
            lastSendTime_ = GetMicrosecondTimestamp();
        }
    }

    // Check if we should trigger temporary rate increase based on replica sequence number
    if (temporaryRateIncreaseEnabled_ && !rateIncreaseActive_ && replicaSeq >= rateIncreaseSeqThreshold_ &&
        rateIncreaseStartTime_ == 0) {
        rateIncreaseActive_ = true;
        rateIncreaseStartTime_ = GetMicrosecondTimestamp();
        LOG(INFO) << "Triggering temporary rate increase at replica sequence " << replicaSeq;
        LOG(INFO) << "  Send rate: " << sendRate_ << " -> " << increasedSendRate_;
        LOG(INFO) << "  Max in flight: " << maxInFlight_ << " -> " << increasedMaxInFlight_;
    }

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

        // Normal path timeout, if we have received cert, and
        if (reqState.collector.hasCert() && !reqState.certSent && now - reqState.certTime > normalPathTimeout_ &&
            normalPathEnabled_) {
            VLOG(2) << "Request number " << clientSeq << " fast path timed out! Sending cert!";
            reqState.certSent = true;

            // Send cert to replicas;
            endpoint_->PrepareProtoMsg(reqState.collector.getCert(), CERT);
            for (const Address &addr : replicaAddrs_) {
                endpoint_->SendPreparedMsgTo(addr);
            }
            continue;
        }

        if (reqState.triggerSent && now - reqState.triggerSendTime > requestTimeout_) {
            // This is expected to happen when the replicas are making progress without the client's request
            LOG(INFO) << "Client repair on request " << clientSeq << " timed out again, retrying request through DOM";
            ClientRequest &req = reqState.request;
            req.set_send_time(now);

            reqState.sendTime = now;

            reqState.triggerSent = false;
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

        bool verified = false;
        if (useHMAC_) {
            verified = hmacProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()});

        } else {
            verified = sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()});
        }

        if (!verified) {
            LOG(INFO) << "Failed to verify replica signature from " << reply.replica_id();
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

        if (!sigProvider_.verify(hdr, {NodeType::REPLICA, certReply.replica_id()})) {
            LOG(INFO) << "Failed to verify replica signature for CERT_REPLY!";
            return;
        }

        handleCertReply(certReply, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    else if (hdr->msgType == MessageType::COMMITTED_REPLY) {
        CommittedReply reply;

        if (!reply.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse COMMITTED_REPLY message";
            return;
        }

        if (!sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()})) {
            LOG(INFO) << "Failed to verify replica signature for COMMITTED_REPLY!";
            return;
        }

        handleCommittedReply(reply, std::span{body + hdr->msgLen, hdr->sigLen});
    } else if (hdr->msgType == MessageType::REPAIR_SUMMARY) {
        RepairSummary repairSummary;

        if (!repairSummary.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse REPAIR_SUMMARY message";
            return;
        }

        if (!sigProvider_.verify(hdr, {NodeType::REPLICA, repairSummary.replica_id()})) {
            LOG(INFO) << "Failed to verify replica signature for REPAIR_SUMMARY!";
            return;
        }

        handleRepairSummary(repairSummary, std::span{body + hdr->msgLen, hdr->sigLen});
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

    VLOG(4) << "Received reply from replica " << reply.replica_id() << " round " << reply.round() << " for c_seq "
            << clientSeq << " at log pos " << reply.seq() << " after " << now - reqState.sendTime << " usec";

    bool hasCertBefore = reqState.collector.hasCert();
    uint32_t maxMatchSize = reqState.collector.insertReply(reply, std::vector<byte>(sig.begin(), sig.end()));

    if (reqState.collector.numReceived() == quorumSize_) {
        reqState.quorumTime = now;
    }

    // Just collected cert
    if (!hasCertBefore && reqState.collector.hasCert()) {
        VLOG(2) << "Created cert for request number " << clientSeq;
        reqState.certTime = now;
    }

    if (maxMatchSize >= superQuorumSize_) {
        // TODO Deliver to application
        // Request is committed and can be cleaned up.
        VLOG(1) << "PERF event=commit path=fast" << " client_id=" << clientId_ << " client_seq=" << clientSeq
                << " seq=" << reply.seq() << " round=" << reply.round() << " latency=" << now - reqState.firstSendTime
                << " digest=" << digest_to_hex(reply.digest()) << " queued=" << reply.queued();

        commitRequest(clientSeq, reply.seq());
        return;
    }

    // `hasCert() == true` iff maxMatchSize >= quorumSize_
    // TODO handle sending cert in new round better
    if (!reqState.certSent && reqState.collector.hasCert() && reqState.collector.numReceived() > maxMatchSize &&
        normalPathEnabled_) {
        LOG(INFO) << "Request number " << clientSeq << " fast path impossible, has cert. Sending cert!";
        reqState.certSent = true;

        // Send cert to replicas
        endpoint_->PrepareProtoMsg(reqState.collector.getCert(), CERT);
        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }

    bool sendProofs = ConfigManager::getInstance().getConfig().clientSendProofs;

    // If the number of potential remaining replies is not enough to reach 2f + 1 for any matching reply,
    // we have a proof of inconsistency.
    if (sendProofs && !reqState.triggerSent && reqState.collector.numReceived() - maxMatchSize > f_ &&
        reqState.collector.round_ == reply.round()) {
        LOG(INFO) << "Client detected cert is impossible, triggering repair with proof for cseq=" << clientSeq
                  << " for round=" << reqState.collector.round_;

        reqState.triggerSent = true;
        reqState.triggerRound = reqState.collector.round_;
        reqState.triggerSendTime = now;

        RepairReplyProof proofMsg;

        proofMsg.set_round(reqState.collector.round_);

        for (auto &[replicaId, r] : reqState.collector.replies_) {
            if (r.round() != reqState.collector.round_)
                continue;

            auto &sig = reqState.collector.signatures_[replicaId];
            proofMsg.add_signatures(std::string(sig.begin(), sig.end()));
            (*proofMsg.add_replies()) = r;
        }
        reqState.triggerSendTime = GetMicrosecondTimestamp();
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(proofMsg, REPAIR_REPLY_PROOF);   // JK: Unused?
        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }
}

void Client::handleCertReply(const CertReply &certReply, std::span<byte> sig)
{
    uint32_t cseq = certReply.client_seq();

    assert(normalPathEnabled_);

    if (requestStates_.count(cseq) == 0) {
        // VLOG(2) << "Received certReply for " << cseq << " not in active requests";
        return;
    }

    auto &reqState = requestStates_.at(cseq);
    reqState.certReplies.insert(certReply.replica_id());

    VLOG(4) << "Received cert ack client_seq=" << cseq << " seq=" << certReply.seq() << " round=" << certReply.round()
            << " replica_id=" << certReply.replica_id();

    if (reqState.certReplies.size() >= quorumSize_) {
        VLOG(1) << "PERF event=commit path=normal client_id=" << clientId_ << " client_seq=" << cseq
                << " seq=" << certReply.seq() << " round=" << certReply.round()
                << " latency=" << GetMicrosecondTimestamp() - reqState.firstSendTime
                << " digest=" << digest_to_hex(reqState.collector.cert_->replies()[0].digest());
        commitRequest(cseq, certReply.seq());
    }
}

void Client::handleCommittedReply(const dombft::proto::CommittedReply &reply, std::span<byte> sig)
{
    if (reply.client_id() != clientId_)
        return;

    uint32_t cseq = reply.client_seq();

    if (requestStates_.count(cseq) == 0)
        return;

    auto &reqState = requestStates_.at(cseq);

    reqState.repairReplies.insert(reply.replica_id());
    if (reqState.repairReplies.size() >= f_ + 1) {
        // Request is committed, so we can clean up state!
        // TODO check we have a consistent set of application replies!

        if (reply.is_repair()) {
            VLOG(1) << "PERF event=commit path=slow client_id=" << clientId_ << " client_seq=" << cseq
                    << " seq=" << reply.seq() << " latency=" << GetMicrosecondTimestamp() - reqState.firstSendTime;
        } else {
            VLOG(1) << "PERF event=commit path=missed client_id=" << clientId_ << " client_seq=" << cseq
                    << " seq=" << reply.seq() << " latency=" << GetMicrosecondTimestamp() - reqState.firstSendTime;
        }

        commitRequest(cseq, reply.seq());
    }
}

void Client::handleRepairSummary(const dombft::proto::RepairSummary &summary, std::span<byte> sig)
{
    VLOG(2) << "Received repair summary for round=" << summary.round() << " from replicaId=" << summary.replica_id();

    ::ClientSequence committed(summary.committed_seqs());
    for (const auto &[cseq, reqState] : requestStates_) {
        // TODO this is a hack to make a dummy reply...
        if (!committed.contains(cseq)) {
            continue;
        }

        CommittedReply reply;
        reply.set_client_id(clientId_);
        reply.set_client_seq(cseq);
        reply.set_is_repair(false);   // Missed in this round
        reply.set_seq(0);             // TODO we do not know the seq
        reply.set_replica_id(summary.replica_id());

        LOG(INFO) << "DEBUG adding request that was in repair summary checkpoint record";
    }

    for (const CommittedReply &reply : summary.replies()) {
        if (reply.client_id() != clientId_)
            continue;

        VLOG(4) << "Repair summary reply client_id=" << reply.client_id() << " client_seq=" << reply.client_seq()
                << " seq=" << reply.seq() << " replica_id=" << reply.replica_id();

        handleCommittedReply(reply, sig);
    }
}

}   // namespace dombft
