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

        endpoint_ = std::make_unique<NngEndpoint>(addrPairs, true);
    } else {
        replicaAddr_ =
            (Address(config.receiverLocal ? "127.0.0.1" : config.replicaIps[receiverId], config.replicaPort));
        endpoint_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort, false);
        forwardEp_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort+100, false);
    }

    LOG(INFO) << "Bound replicaAddr_=" << replicaAddr_.GetIPAsString() << ":" << replicaAddr_.GetPortAsInt();

    fwdTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Receiver *) ctx)->checkDeadlines(); }, 1000, this);

    endpoint_->RegisterTimer(fwdTimer_.get());
    endpoint_->RegisterMsgHandler([this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->receiveRequest(msgHdr, msgBuffer, sender);
    });
}

Receiver::~Receiver()
{
    // TODO cleanup...
}

void Receiver::run()
{
    // Submit first request
    LOG(INFO) << "Starting event loop...";
    endpoint_->LoopRun();
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

    DOMRequest request;
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

        MessageHeader *hdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(Address(sender->GetIPAsString(), proxyMeasurementPort_));

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
            VLOG(3) << "Adding request to priority queue with deadline=" << request.deadline() << " in "
                    << request.deadline() - recv_time << "us";
            deadlineQueue_[{request.deadline(), request.client_id()}] = request;

            // Check if timer is firing before deadline
            uint64_t now = GetMicrosecondTimestamp();
            uint64_t nextCheck = request.deadline() - now;

            if (nextCheck <= endpoint_->GetTimerRemaining(fwdTimer_.get())) {
                endpoint_->ResetTimer(fwdTimer_.get(), nextCheck);
                VLOG(3) << "Changed next deadline check to be in " << nextCheck << "us";
            }
        }
    }
}

void Receiver::forwardRequest(const DOMRequest &request)
{
    if (false)   // receiverConfig_.ipcReplica)
    {
        // TODO
        throw "IPC communciation not implemented";
    } else {
        VLOG(1) << "Forwarding Request with deadline " << request.deadline() << " to " << replicaAddr_.GetIPAsString()
                << " c_id=" << request.client_id() << " c_seq=" << request.client_seq();

        MessageHeader *hdr = forwardEp_->PrepareProtoMsg(request, MessageType::DOM_REQUEST);
        // TODO check errors for all of these lol
        // TODO do this while waiting, not in the critical path

#if FABRIC_CRYPTO
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
        forwardEp_->SendPreparedMsgTo(replicaAddr_);
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

    endpoint_->ResetTimer(fwdTimer_.get(), nextCheck);
}

}   // namespace dombft