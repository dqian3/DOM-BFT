#include "receiver.h"

#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <openssl/pem.h>

namespace dombft {
using namespace dombft::proto;

Receiver::Receiver(const ProcessConfig &config, uint32_t receiverId, bool skipForwarding, bool ignoreDeadlines)
    : receiverId_(receiverId)
    , proxyMeasurementPort_(config.proxyMeasurementPort)
    , skipForwarding_(skipForwarding)
    , ignoreDeadlines_(ignoreDeadlines)
    , running_(true)
{
    std::string receiverIp = config.receiverIps.at(receiverId_);
    LOG(INFO) << "receiverIP=" << receiverIp;
    int receiverPort = config.receiverPort;
    LOG(INFO) << "receiverPort=" << receiverPort;

    std::string receiverKey = config.receiverKeysDir + "/receiver" + std::to_string(receiverId_) + ".der";
    LOG(INFO) << "Loading key from " << receiverKey;
    if (!sigProvider_.loadPrivateKey(receiverKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("proxy", config.proxyKeysDir)) {
        LOG(ERROR) << "Unable to load proxy public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("client", config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    /** Store replica addrs */
    numReceivers_ = config.receiverIps.size();
    if (config.transport == "nng") {
        auto addrPairs = getReceiverAddrs(config, receiverId);
        replicaAddr_ = addrPairs.back().second;
        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true);
    } else {
        replicaAddr_ = Address(config.replicaIps[receiverId], config.replicaPort);
        LOG(INFO) << "Replica Address: " << replicaAddr_;

        endpoint_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort, true);
    }

    fwdTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Receiver *) ctx)->checkDeadlines(); }, 1000, this);

    ev_set_priority(fwdTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(fwdTimer_.get());
    endpoint_->RegisterMsgHandler([this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->receiveRequest(msgHdr, msgBuffer, sender);
        checkDeadlines();
    });
    endpoint_->RegisterSignalHandler([&]() {
        running_ = false;
        endpoint_->LoopBreak();
    });

    // Start verify threads
    // TODO parameterize verifyThreads
    uint32_t numVerifyThreads = config.numVerifyThreads;

    for (int i = 0; i < numVerifyThreads; i++) {
        verifyThds_.emplace_back(&Receiver::verifyThd, this, i);
    }

    endpoint_->LoopRun();
    for (std::thread &thd : verifyThds_) {
        thd.join();
    }
    LOG(INFO) << "Receiver exited cleanly";
}

Receiver::~Receiver() {}

void Receiver::receiveRequest(MessageHeader *hdr, byte *body, Address *sender)
{
    if (hdr->msgLen < 0) {
        return;
    }

#if FABRIC_CRYPTO
    if (!sigProvider_.verify(hdr, "proxy", 0)) {
        LOG(INFO) << "Failed to verify proxy signature";
        return;
    }
#endif

    // We don't expect any other kind of message.
    if (hdr->msgType != MessageType::DOM_REQUEST) {
        LOG(ERROR) << "Received message type " << hdr->msgType << " != DOM_REQUEST";
        return;
    }

    DOMRequest request;
    if (!request.ParseFromArray(body, hdr->msgLen)) {
        LOG(ERROR) << "Unable to parse DOM_REQUEST message";
        return;
    }

#if FABRIC_CRYPTO
    // TODO: verify sending from proxy.
#endif

    int64_t recv_time = GetMicrosecondTimestamp();
    VLOG(3) << "RECEIVE c_id=" << request.client_id() << " c_seq=" << request.client_seq() << " Measured delay "
            << recv_time << " - " << request.send_time() << " = " << recv_time - request.send_time() << " usec";

    if (recv_time > request.deadline()) {
        request.set_late(true);

        VLOG(1) << "Request is late by " << recv_time - request.deadline() << "us";
    }

    uint64_t deadline = request.deadline();
    if (ignoreDeadlines_) {
        // This will just make the receiver forward messages in order of receiving
        deadline = recv_time;
    }

    auto r = std::make_shared<Request>(request, request.deadline(), request.client_id(), false);

    {
        std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
        deadlineQueue_[{deadline, request.client_id()}] = r;
    }

    verifyQueue_.enqueue(r);

    // Send measurements replies back to the proxy, but only every 5ms
    if (recv_time - lastMeasurementTimes_[request.proxy_id()] > 5000) {
        lastMeasurementTimes_[request.proxy_id()] = recv_time;

        MeasurementReply mReply;
        mReply.set_receiver_id(receiverId_);
        mReply.set_owd(recv_time - request.send_time());
        mReply.set_send_time(request.send_time());
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY);
#if FABRIC_CRYPTO
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
        endpoint_->SendPreparedMsgTo(Address(sender->ip(), proxyMeasurementPort_), hdr);
    }
}

void Receiver::forwardRequest(const DOMRequest &request)
{
    uint64_t now = GetMicrosecondTimestamp();

    if (VLOG_IS_ON(2)) {
        VLOG(2) << "Forwarding request " << now - request.deadline() << "us after deadline r_id=" << receiverId_
                << " c_id=" << request.client_id() << " c_seq=" << request.client_seq();

        if (lastFwdDeadline_ > request.deadline()) {
            VLOG(2) << "Forwarded request out of order!";
        }
    } else if (VLOG_IS_ON(1)) {
        if (numForwarded_ % 10000 == 0) {
            if (numForwarded_ > 0) {
                VLOG(1) << "Forwarded request number " << numForwarded_
                        << " txput=" << 1e+4 * 1e+6 / (now - lastStatTime_) << " req/s took " << now - lastStatTime_
                        << " usec queue_size=" << deadlineQueue_.size();
            }
            lastStatTime_ = now;
        }
    }

    numForwarded_ += 1;
    lastFwdDeadline_ = request.deadline();

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::DOM_REQUEST);
#if FABRIC_CRYPTO
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
    if (skipForwarding_) {
        return;
    }

    endpoint_->SendPreparedMsgTo(replicaAddr_, hdr);
}

void Receiver::checkDeadlines()
{
    std::lock_guard<std::mutex> guard(deadlineQueueMtx_);

    uint64_t now = GetMicrosecondTimestamp();
    auto it = deadlineQueue_.begin();

    // ->first gets the key of {deadline, client_id}, second .first gets deadline
    while (it != deadlineQueue_.end() && it->first.first <= now) {
        VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;

        if (!it->second->verified) {
            VLOG(3) << "Request not verified, waiting for next check";
            break;
        }

        forwardRequest(it->second->request);
        auto temp = std::next(it);
        deadlineQueue_.erase(it);
        it = temp;
    }

    int64_t nextCheck = deadlineQueue_.empty() ? 1000 : (int64_t) deadlineQueue_.begin()->first.first - now;
    nextCheck = std::max(1000l, nextCheck);

    endpoint_->ResetTimer(fwdTimer_.get(), nextCheck);
}

void Receiver::verifyThd(int workerId)
{
    LOG(INFO) << "Starting verify thd";

    u_int32_t numVerified = 0;
    std::shared_ptr<Request> request;
    while (running_) {

        if (!verifyQueue_.wait_dequeue_timed(request, 10000)) {
            continue;
        }

        ClientRequest clientHeader;

        // Separate this out into another function probably.

        // TODO is there a race condition reading the request here?
        MessageHeader *clientMsgHdr = (MessageHeader *) request->request.client_req().c_str();
        byte *clientBody = (byte *) (clientMsgHdr + 1);
        if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            return;
        }

        bool verified = sigProvider_.verify(clientMsgHdr, "client", request->clientId);

        {
            std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
            if (verified) {
                VLOG(4) << "Verified client signature for c_id=" << request->clientId
                        << " c_seq=" << request->request.client_seq() << " time until deadline: "
                        << ((int64_t) request->request.deadline()) - GetMicrosecondTimestamp() << " us";
                request->verified = true;
            } else {
                VLOG(1) << "Failed to verify client signature!";
                deadlineQueue_.erase({request->deadline, request->clientId});
            }
        }

        numVerified++;
    }

    LOG(INFO) << "Worker " << workerId << " verified " << numVerified << " client requests";
}

}   // namespace dombft