#include "receiver.h"

#include <openssl/pem.h>

namespace dombft
{
    using namespace dombft::proto;

    Receiver::Receiver(const std::string &configFile)
    {
        LOG(INFO) << "Loading config information from " << configFile;
        std::string error = receiverConfig_.parseConfig(configFile);
        if (error != "")
        {
            LOG(ERROR) << "Error loading receiver config: " << error << " Exiting.";
            exit(1);
        }

        std::string receiverIp = receiverConfig_.receiverIp;
        LOG(INFO) << "receiverIP=" << receiverIp;
        int receiverPort = receiverConfig_.receiverPort;
        LOG(INFO) << "receiverPort=" << receiverPort;

        
        if(!sigProvider_.loadPrivateKey(receiverConfig_.receiverKey)) {
            LOG(ERROR) << "Unable to load private key!";
            exit(1);
        }
        

        if (!sigProvider_.loadPublicKeys("proxy", receiverConfig_.proxyPubKeyPrefix)) {
            LOG(ERROR) << "Unable to load proxy public keys!";
            exit(1);
        }

        
        /** Store all replica addrs */
        for (uint32_t i = 0; i < receiverConfig_.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(receiverConfig_.replicaIps[i],
                                            receiverConfig_.replicaPort));
        }

        endpoint_ = new UDPEndpoint(receiverIp, receiverPort, true);
        msgHandler_ = new UDPMessageHandler(
            [](MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Receiver *)ctx)->receiveRequest(msgHdr, msgBuffer, sender);
            },
            this);

        fwdTimer_ = new Timer(
            [](void *ctx, void * endpoint) {
                ((Receiver *)ctx)->checkDeadlines();
            },
            1000,
            this
        );

        endpoint_->RegisterTimer(fwdTimer_);
        endpoint_->RegisterMsgHandler(msgHandler_);
    }

    Receiver::~Receiver()
    {
        // TODO cleanup...
    }

    void Receiver::Run()
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
        
        if (!sigProvider_.verify(hdr, body, "proxy", 0)){
            LOG(INFO) << "Failed to verify proxy signature";
            return;
        }

        DOMRequest request;
        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            // TODO verify and handle signed header better
            if (!request.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            // Send measurement reply right away
            uint64_t recv_time = GetMicrosecondTimestamp();

            MeasurementReply mReply;
            mReply.set_receiver_id(receiverConfig_.receiverId);
            mReply.set_owd(recv_time - request.send_time());
            VLOG(3) << "Measured delay: " << mReply.owd();

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY);
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
            endpoint_->SendPreparedMsgTo(Address(sender->GetIPAsString(), receiverConfig_.proxyMeasurementPort));
        
            // Check if request is on time.
            request.set_late(recv_time > request.deadline());

            if (request.late()){
                VLOG(3) << "Request is late, sending immediately";
                forwardRequest(request);
            } else {
                VLOG(3) << "Adding request to priority queue with deadline " << request.deadline() 
                << " in " << request.deadline() - recv_time << "us";
                deadlineQueue_[{request.deadline(), request.client_id()}] = request;
            }

        }
    }

    void Receiver::forwardRequest(const DOMRequest &request)
    {
        if (receiverConfig_.ipcReplica)
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
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);

            for (const Address &addr : replicaAddrs_)
            {
                endpoint_->SendPreparedMsgTo(addr, true);
            }
            endpoint_->setBufReady(false);
        }
    }


    // TODO: there is probably a smarter way than just having this poll every so often
    void Receiver::checkDeadlines()
    {
        uint64_t now = GetMicrosecondTimestamp();

        auto it = deadlineQueue_.begin();

        // ->first gets the key of {deadline, client_id}, second .first gets deadline
        while (it != deadlineQueue_.end() && it->first.first <= now) {
            VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;
            forwardRequest(it->second);
            auto temp = std::next(it);
            deadlineQueue_.erase(it);
            it = temp;
        }
    }

} // namespace dombft