#include "client.h"


namespace dombft
{
    using namespace dombft::proto;

    Client::Client(const std::string &configFile)
    {
        LOG(INFO) << "Loading config information from " << configFile;
        std::string error = clientConfig_.parseConfig(configFile);
        if (error != "")
        {
            LOG(ERROR) << "Error loading client config: " << error << " Exiting.";
            exit(1);
        }
        clientId_ = clientConfig_.clientId;
        LOG(INFO) << "clientId=" << clientId_;
        std::string clientIP = clientConfig_.clientIp;
        LOG(INFO) << "clientIP=" << clientIP;
        int clientPort = clientConfig_.clientPort;
        LOG(INFO) << "clientPort=" << clientPort;




        /** Store all proxy addrs. TODO handle mutliple proxy sockets*/
        for (uint32_t i = 0; i < clientConfig_.proxyIps.size(); i++)
        {
            LOG(INFO) << "Proxy " << i + 1 << ": " <<  clientConfig_.proxyIps[i] << ", " << clientConfig_.proxyPortBase;
            proxyAddrs_.push_back(Address(clientConfig_.proxyIps[i],
                                            clientConfig_.proxyPortBase));  
        }


        /** Generate zipfian workload */
        // LOG(INFO) << "keyNum=" << clientConfig_.keyNum
        //           << "\tskewFactor=" << clientConfig_.skewFactor
        //           << "\twriteRatio=" << clientConfig_.writeRatio;
        // zipfianKeys_.resize(1000000, 0);
        // retryTimeoutUs_ = clientConfig_.requestRetryTimeUs;
        // if (clientConfig_.keyNum > 1)
        // {
        //     std::default_random_engine generator(clientId_); // clientId as the seed
        //     zipfian_int_distribution<uint32_t> zipfianDistribution(
        //         0, clientConfig_.keyNum - 1, clientConfig_.skewFactor);
        //     for (uint32_t i = 0; i < zipfianKeys_.size(); i++)
        //     {
        //         zipfianKeys_[i] = zipfianDistribution(generator);
        //     }
        // }



        /* Setup keys */
        if (!sigProvider_.loadPrivateKey(clientConfig_.clientKey)) {
            LOG(ERROR) << "Error loading client private key, exiting...";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("replica", clientConfig_.replicaKeysDir)) {
            LOG(ERROR) << "Error loading replica public keys, exiting...";
            exit(1);
        }

        /** Initialize state */
        nextReqSeq_ = 1;

        endpoint_ = new UDPEndpoint(clientIP, clientPort, true);
        replyHandler_ = new UDPMessageHandler(
            [] (MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Client *)ctx)->ReceiveReply(msgHdr, msgBuffer, sender);
            },
            this
        );

        endpoint_->RegisterMsgHandler(replyHandler_);
    }

    Client::~Client()
    {
        // TODO cleanup... though we don't really reuse this
    }

    void Client::Run()
    {
        // Submit first request
        SubmitRequest();
        endpoint_->LoopRun();
    }

    void Client::ReceiveReply(MessageHeader *msgHdr, byte *msgBuffer,
                              Address *sender)
    {
        if (msgHdr->msgLen < 0)
        {
            return;
        }
        Reply reply;
        if (msgHdr->msgType == MessageType::REPLY || msgHdr->msgType == MessageType::FAST_REPLY)
        {
            // TODO verify and handle signed header better
            if (!reply.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                return;
            }

            // TODO handle dups
            numReplies_++;
            if (reply.fast())
            {
                numFastReplies_++;
            }

            // if (numReplies_ == )

        }
    }


    void Client::SubmitRequest()
    {
        if (false) // TODO nextReqSeq_ != commitedId + 1, if called when there is still a pending request
        {
            LOG(ERROR) << "SubmitRequest() called before request completed!";
            return;
        }
        ClientRequest request;

        // submit new request
        request.set_client_id(clientId_);
        request.set_client_seq(nextReqSeq_);
        request.set_send_time(GetMicrosecondTimestamp());
        request.set_is_write(true); // TODO modify this based on some random chance

        // TODO, select a proxy or replica based on useProxy
        Address &addr = proxyAddrs_[0];

        // TODO maybe client should own the memory instead of proxy.
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(addr);
        VLOG(1) << "Sent request number " << nextReqSeq_ << " to " << addr.GetIPAsString();

        nextReqSeq_++;

        // TODO record this outstanding request somewhere
    }


} // namespace dombft