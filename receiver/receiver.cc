#include "receiver/receiver.h"


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
        PEM_read_bio_PrivateKey(bo, &key, 0, 0 );
        BIO_free(bo);

        if(key == NULL) {
            LOG(ERROR) << "Unable to load receiver private key!";
            exit(1);
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < receiverConfig_.replicaIps.size(); i++)
        {
            std::cout << receiverConfig_.replicaIps[i] << std::endl;
            replicaAddrs_.push_back(Address(receiverConfig_.replicaIps[i],
                                            receiverConfig_.replicaPort));  
        }


        endpoint_ = new SignedUDPEndpoint(receiverIp, receiverPort, key, true);
        replyHandler_ = new UDPMsgHandler(
            [] (MessageHeader *msgHdr, char *msgBuffer, Address *sender, void *ctx)
            {
                ((Receiver *)ctx)->ReceiveRequest(msgHdr, msgBuffer, sender);
            },
            this
        );

        endpoint_->RegisterMsgHandler(replyHandler_);
    }

    Receiver::~Receiver()
    {
        // TODO cleanup... though we don't really reuse this
    }

    void Receiver::Run()
    {
        // Submit first request
        LOG(INFO) << "Starting event loop...";
        endpoint_->LoopRun();
    }

    void Receiver::ReceiveRequest(MessageHeader *msgHdr, char *msgBuffer,
                              Address *sender)
    {
        if (msgHdr->msgLen < 0)
        {
            return;
        }
        DOMRequest request;
        if (msgHdr->msgType == MessageType::DOM_REQUEST)
        {
            // TODO verify and handle signed header better
            if (!request.ParseFromArray(msgBuffer + sizeof(SignedMessageHeader), msgHdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                return;
            }


            // Send measurement reply right away
            uint64_t recv_time = GetMicrosecondTimestamp();

            MeasurementReply mReply;
            mReply.set_receiver_id(receiverConfig_.receiverId);
            mReply.set_owd(recv_time - request.send_time());
            
            endpoint_->SignAndSendProtoMsgTo(*sender, mReply, MessageType::MEASUREMENT_REPLY);

            // Check if request is on time.
            request.set_late(recv_time < request.deadline());

            // Forward request

            if (receiverConfig_.ipcReplica) {
                // TODO
                throw "IPC communciation not implemented";
            } else {

                VLOG(1) << "Forwarding Request with deadline " << request.deadline();

                for (const Address &addr: replicaAddrs_) {
                    endpoint_->SignAndSendProtoMsgTo(addr, request, MessageType::DOM_REQUEST);
                }
            }

        }
    }




} // namespace dombft