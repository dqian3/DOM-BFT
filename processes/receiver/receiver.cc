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

        BIO *bo = BIO_new_file(receiverConfig_.receiverKey.c_str(), "r");
        EVP_PKEY *key = NULL;
        PEM_read_bio_PrivateKey(bo, &key, 0, 0);
        BIO_free(bo);

        if (key == NULL)
        {
            LOG(ERROR) << "Unable to load receiver private key!";
            exit(1);
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < receiverConfig_.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(receiverConfig_.replicaIps[i],
                                            receiverConfig_.replicaPort));
        }

        endpoint_ = new SignedUDPEndpoint(receiverIp, receiverPort, key, true);
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
        DOMRequest request;
        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            SignedMessageHeader *shdr = (SignedMessageHeader *) body;
            u_char *reqBytes = (u_char *)(shdr + 1);

            // TODO verify and handle signed header better
            if (!request.ParseFromArray(reqBytes, hdr->msgLen - shdr->sigLen - sizeof(SignedMessageHeader)))
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

            endpoint_->SignAndSendProtoMsgTo(Address(sender->GetIPAsString(), receiverConfig_.proxyMeasurementPort), mReply, MessageType::MEASUREMENT_REPLY);
        
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

            for (const Address &addr : replicaAddrs_)
            {
                endpoint_->SignAndSendProtoMsgTo(addr, request, MessageType::DOM_REQUEST);
            }
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