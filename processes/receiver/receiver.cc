#include "receiver.h"

#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <openssl/pem.h>

namespace dombft {
using namespace dombft::proto;

Receiver::Receiver(const ProcessConfig &config, uint32_t receiverId)
    : receiverId_(receiverId)
    , proxyMeasurementPort_(config.proxyMeasurementPort)
{
    std::string receiverIp = config.receiverIps[receiverId_];
    LOG(INFO) << "receiverIP=" << receiverIp;
    int receiverPort = config.receiverPort;
    LOG(INFO) << "receiverPort=" << receiverPort;

    std::string receiverKey = config.receiverKeysDir + "/receiver" + std::to_string(receiverId_) + ".pem";
    LOG(INFO) << "Loading key from " << receiverKey;
    if (!sigProvider_.loadPrivateKey(receiverKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("proxy", config.proxyKeysDir)) {
        LOG(ERROR) << "Unable to load proxy public keys!";
        exit(1);
    }

    /** Store replica addrs */

    if (config.transport == "nng") {
        auto addrPairs = getReceiverAddrs(config, receiverId);
        replicaAddr_ = addrPairs.back().second;

        std::vector<std::pair<Address, Address>> recvAddrs(addrPairs.begin(),
                                                              addrPairs.end() - config.proxyIps.size());
        std::vector<std::pair<Address, Address>> forwardAddrs(addrPairs.end() - config.proxyIps.size(),
                                                                 addrPairs.end());

        recvReqEp_ = std::make_unique<NngEndpoint>(recvAddrs, false);

        forwardReqEp_ = std::make_unique<NngEndpoint>(forwardAddrs, true);
    } else {
        replicaAddr_ =
            (Address(config.receiverLocal ? "127.0.0.1" : config.replicaIps[receiverId], config.replicaPort));
        recvReqEp_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort, false);
        forwardReqEp_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort, true);
    }

    LOG(INFO) << "Bound replicaAddr_=" << replicaAddr_.GetIPAsString() << ":" << replicaAddr_.GetPortAsInt();
}

Receiver::~Receiver()
{
    // TODO cleanup...
}

void Receiver::terminate()
{
    LOG(INFO) << "terminating the receiver...";
    running_ = false;
}

void Receiver::run()
{
    // Submit first request
    LOG(INFO) << "Starting event loop...";

    running_ = true;

    LaunchThreads();

}

void Receiver::LaunchThreads() 
{
    threads_["receiveRequestTd"] = new std::thread(&Receiver::receiveRequestTd, this);
    threads_["forwardRequestTd"] = new std::thread(&Receiver::forwardRequestTd, this);
}

void Receiver::receiveRequestTd()
{
    auto checkEnd = [](void* ctx, void*receiverEP) {
        if (!((Receiver *) ctx)->running_) {
            ((Endpoint *) receiverEP)->LoopBreak();
        }
    };

    Timer monitor(checkEnd, 10, this);

    recvReqEp_->RegisterMsgHandler([this](MessageHeader *msghdr, byte *msgBuffer, Address *sender) {
        this->receiveRequest(msghdr, msgBuffer, sender);
    });
    recvReqEp_->RegisterTimer(&monitor);

    recvReqEp_->LoopRun();

}

void Receiver::forwardRequestTd()
{
    fwdTimer_ = std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Receiver *)ctx) -> checkDeadlines(); }, 1000, this);
    forwardReqEp_->RegisterTimer(fwdTimer_.get());

    checkQueueTimer_ = std::make_unique<Timer> ([](void* ctx, void *enpoint) { ((Receiver *) ctx) -> checkRecvQueue(); }, 1000, this);
    forwardReqEp_->RegisterTimer(checkQueueTimer_.get());

    auto checkEnd = [](void* ctx, void*receiverEP) {
        if (!((Receiver *) ctx)->running_) {
            ((Endpoint *) receiverEP)->LoopBreak();
        }
    };

    Timer monitor(checkEnd, 10, this);

    recvReqEp_->RegisterTimer(&monitor);

    forwardReqEp_->LoopRun();
}


void Receiver::receiveRequest(MessageHeader *hdr, byte *body, Address *sender)
{
    if (hdr->msgLen < 0) {
        return;
    }

#if FABRIC_CRYPTO
    if (!sigProvider_.verify(hdr, body, "proxy", 0)) {
        LOG(INFO) << "Failed to verify proxy signature";
        return;
    }
#endif

    dombft::proto::DOMRequest request;
    if (hdr->msgType == MessageType::DOM_REQUEST) {
        if (!request.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DOM_REQUEST message";
            return;
        }

#if FABRIC_CRYPTO
        // TODO: verify sending from proxy.
#endif

        // Send measurement reply right away
        int64_t recv_time = GetMicrosecondTimestamp();

        MeasurementReply mReply;
        mReply.set_receiver_id(receiverId_);
        mReply.set_owd(recv_time - request.send_time());
        VLOG(3) << "Measured delay " << recv_time << " - " << request.send_time() << " = " << mReply.owd() << " usec";

        MessageHeader *hdr = recvReqEp_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        recvReqEp_->SendPreparedMsgTo(Address(sender->GetIPAsString(), proxyMeasurementPort_));

        // Check if request is on time.
        request.set_late(recv_time > request.deadline());

        VLOG(4) << "Received request c_id=" << request.client_id() << " c_seq=" << request.client_seq()
                << " deadline=" << request.deadline() << " now=" << recv_time;

        if (request.late()) {
            VLOG(3) << "Request is late, sending immediately deadline=" << request.deadline() << " late by "
                    << recv_time - request.deadline() << "us";
            VLOG(3) << "Checking deadlines before forwarding late message";
            checkDeadlines();

            forwardRequest(request);
        } else {
            VLOG(3) << "Recv thread push the request to concurrent queue";
            requestsQueue_.enqueue(request);
        }
    }
}

void Receiver::forwardRequest(const dombft::proto::DOMRequest &request)
{
    if (false)   // receiverConfig_.ipcReplica)
    {
        // TODO
        throw "IPC communciation not implemented";
    } else {
        VLOG(1) << "Forwarding Request with deadline " << request.deadline() << " to " << replicaAddr_.GetIPAsString()
                << " c_id=" << request.client_id() << " c_seq=" << request.client_seq();

        MessageHeader *hdr = forwardReqEp_->PrepareProtoMsg(request, MessageType::DOM_REQUEST);
        // TODO check errors for all of these lol
        // TODO do this while waiting, not in the critical path

#if FABRIC_CRYPTO
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
        forwardReqEp_->SendPreparedMsgTo(replicaAddr_);
    }
}

void Receiver::checkDeadlines()
{
    uint64_t now = GetMicrosecondTimestamp();

    auto it = deadlineQueue_.begin();

    VLOG(3) << "Checking deadlines";

    // ->first gets the key of {deadline, client_id}, second .first gets deadline
    while (it != deadlineQueue_.end() && it->first.first <= now) {
        VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;
        forwardRequest(it->second);
        auto temp = std::next(it);
        deadlineQueue_.erase(it);
        it = temp;
    }

    uint32_t nextCheck = deadlineQueue_.empty() ? 10000 : deadlineQueue_.begin()->first.first - now;
    VLOG(3) << "Next deadline check in " << nextCheck << "us";

    forwardReqEp_->ResetTimer(fwdTimer_.get(), nextCheck);
}

void Receiver::checkRecvQueue()
{
    dombft::proto::DOMRequest request;
    while (requestsQueue_.try_dequeue(request)) {
        int64_t recv_time = GetMicrosecondTimestamp();

        VLOG(3) << "fetching request from the concurrent queue with deadline= " << request.deadline() << " in "
                    << request.deadline() - recv_time << "us";
        deadlineQueue_[{request.deadline(), request.client_id()}] = request;
    
        // Check if timer is firing before deadline
        uint64_t now = GetMicrosecondTimestamp();
        uint64_t nextCheck = request.deadline() - now;

        if (nextCheck <= forwardReqEp_->GetTimerRemaining(fwdTimer_.get())) {
            forwardReqEp_->ResetTimer(fwdTimer_.get(), nextCheck);
            VLOG(3) << "Changed next deadline check to be in " << nextCheck << "us";
        }
    }
}

}   // namespace dombft