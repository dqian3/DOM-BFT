#include "receiver.h"

#include <openssl/pem.h>

namespace dombft
{
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
        if (!sigProvider_.loadPrivateKey(receiverKey))
        {
            LOG(ERROR) << "Unable to load private key!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("proxy", config.proxyKeysDir))
        {
            LOG(ERROR) << "Unable to load proxy public keys!";
            exit(1);
        }

        /** Store replica addrs */

        if (config.receiverLocal) {
            replicaAddr_ = (Address("127.0.0.1",
                                        config.replicaPort));
        } else {
            replicaAddr_ = (Address(config.replicaIps[receiverId],
                                    config.replicaPort));
        }

        endpoint_ = std::make_unique<UDPEndpoint>(receiverIp, receiverPort, true);
        msgHandler_ = std::make_unique<UDPMessageHandler>(
            [](MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Receiver *)ctx)->receiveRequest(msgHdr, msgBuffer, sender);
            },
            this);

        fwdTimer_ = std::make_unique<Timer>(
            [](void *ctx, void *endpoint)
            {
                ((Receiver *)ctx)->checkDeadlines();
            },
            1000,
            this);

        endpoint_->RegisterTimer(fwdTimer_.get());
        endpoint_->RegisterMsgHandler(msgHandler_.get());
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

    void Receiver::receiveRequest(MessageHeader *hdr, byte *body,
                                  Address *sender)
    {
        if (hdr->msgLen < 0)
        {
            return;
        }

#if FABRIC_CRYPTO
        if (!sigProvider_.verify(hdr, body, "proxy", 0))
        {
            LOG(INFO) << "Failed to verify proxy signature";
            return;
        }
#endif

        DOMRequest request;
        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            if (!request.ParseFromArray(body, hdr->msgLen))
            {
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
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
            endpoint_->SendPreparedMsgTo(Address(sender->GetIPAsString(), proxyMeasurementPort_));

            // Check if request is on time.
            request.set_late(recv_time > request.deadline());

            if (request.late())
            {
                VLOG(3) << "Request is late, sending immediately";
                forwardRequest(request);
            }
            else
            {
                VLOG(3) << "Adding request to priority queue with deadline " << request.deadline()
                        << " in " << request.deadline() - recv_time << "us";
                deadlineQueue_[{request.deadline(), request.client_id()}] = request;
            }
        }
    }

    void Receiver::forwardRequest(const DOMRequest &request)
    {
        if (false) //receiverConfig_.ipcReplica)
        {
            // TODO
            throw "IPC communciation not implemented";
        }
        else
        {
            VLOG(1) << "Forwarding Request with deadline " << request.deadline();

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::DOM_REQUEST);
            // TODO check errors for all of these lol
            // TODO do this while waiting, not in the critical path

#if FABRIC_CRYPTO
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
#endif
            endpoint_->SendPreparedMsgTo(replicaAddr_, true);
            endpoint_->setBufReady(false);
        }
    }

    // TODO: there is probably a smarter way than just having this poll every so often
    void Receiver::checkDeadlines()
    {
        uint64_t now = GetMicrosecondTimestamp();

        auto it = deadlineQueue_.begin();

        // ->first gets the key of {deadline, client_id}, second .first gets deadline
        while (it != deadlineQueue_.end() && it->first.first <= now)
        {
            VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;
            forwardRequest(it->second);
            auto temp = std::next(it);
            deadlineQueue_.erase(it);
            it = temp;
        }
    }

} // namespace dombft