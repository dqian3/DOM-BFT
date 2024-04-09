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
            LOG(INFO) << "Proxy " << i + 1 << ": " << clientConfig_.proxyIps[i] << ", " << clientConfig_.proxyPortBase;
            proxyAddrs_.push_back(Address(clientConfig_.proxyIps[i],
                                          clientConfig_.proxyPortBase));
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < clientConfig_.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(clientConfig_.replicaIps[i],
                                            clientConfig_.replicaPort));
        }

        /* Setup keys */
        if (!sigProvider_.loadPrivateKey(clientConfig_.clientKey))
        {
            LOG(ERROR) << "Error loading client private key, exiting...";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("replica", clientConfig_.replicaKeysDir))
        {
            LOG(ERROR) << "Error loading replica public keys, exiting...";
            exit(1);
        }

        /** Initialize state */
        nextReqSeq_ = 1;

        endpoint_ = new UDPEndpoint(clientIP, clientPort, true);
        replyHandler_ = new UDPMessageHandler(
            [](MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Client *)ctx)->ReceiveReply(msgHdr, msgBuffer, sender);
            },
            this);

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

        if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", 0))
        {
            LOG(INFO) << "Failed to verify replica signature";
            return;
        }

        Reply reply;
        if (msgHdr->msgType == MessageType::REPLY || msgHdr->msgType == MessageType::FAST_REPLY)
        {
            // TODO verify and handle signed header better
            if (!reply.ParseFromArray(msgBuffer, msgHdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse REPLY message";
                return;
            }

            VLOG(3) << "Received reply from replica " << reply.replica_id()
                    << " after " << GetMicrosecondTimestamp() - sendTime_ << " usec";

            // TODO handle dups
            numReplies_++;
            if (reply.fast())
            {
                numFastReplies_++;
            }

            if (numReplies_ >= clientConfig_.replicaIps.size())
            {
                numReplies_ = 0;
                LOG(INFO) << "Fast path commit for " << nextReqSeq_ - 1 << " took "
                          << GetMicrosecondTimestamp() - sendTime_ << " usec";

                numExecuted_++;
                if (numExecuted_ == 100) {
                    exit(0);
                }    

                SubmitRequest();
            }
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

        sendTime_ = request.send_time();

        if (clientConfig_.useProxy) {
            Address &addr = proxyAddrs_[0];

            // TODO maybe client should own the memory instead of proxy.
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
            endpoint_->SendPreparedMsgTo(addr);
            VLOG(1) << "Sent request number " << nextReqSeq_ << " to " << addr.GetIPAsString();
        } else {
            MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
            // TODO check errors for all of these lol
            // TODO do this while waiting, not in the critical path
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);

            for (const Address &addr : replicaAddrs_)
            {
                endpoint_->SendPreparedMsgTo(addr, true);
            }
            endpoint_->setBufReady(false);

        }


        nextReqSeq_++;

        // TODO record this outstanding request somewhere
    }

} // namespace dombft