#include "client.h"


#define NUM_CLIENTS 100

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

        // TODO make this some sort of config
        LOG(INFO) << "Simulating " << clientConfig_.maxInFlight << " simultaneous clients!";
        maxInFlight_ = clientConfig_.maxInFlight;


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
        for (int i = 0; i < maxInFlight_; i++) {
            SubmitRequest();

        }
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

            VLOG(4) << "Received reply from replica " << reply.replica_id()
                    << " after " << GetMicrosecondTimestamp() - sendTimes_[reply.client_seq()] << " usec";

            // TODO handle dups
            numReplies_[reply.client_seq()]++;
            repSeqs_[reply.client_seq()].insert(reply.seq());
            // if (reply.fast())
            // {
            //     numFastReplies_++;
            // }

#if PROTOCOL == PBFT
            if (numReplies_[reply.client_seq()] >= clientConfig_.replicaIps.size() / 3 * 2 + 1)
            {

                LOG(INFO) << "PBFT commit for " << reply.client_seq() - 1 << " took "
                          << GetMicrosecondTimestamp() - sendTimes_[reply.client_seq()] << " usec";

                numReplies_.erase(reply.client_seq());
                sendTimes_.erase(reply.client_seq());

                numExecuted_++;
                if (numExecuted_ >= 1000) {
                    exit(0);
                }    

                SubmitRequest();
            }

#else
            if (numReplies_[reply.client_seq()] >= clientConfig_.replicaIps.size() / 3 * 2 + 1)
            {
                LOG(INFO) << "Fast path commit for " << reply.client_seq() - 1 << " took "
                          << GetMicrosecondTimestamp() - sendTimes_[reply.client_seq()] << " usec";
                
                if (repSeqs_[reply.client_seq()].size() > 1) {
                    LOG(ERROR) << "Contention on sequeunce number " << reply.client_seq() << ", exiting!";
                    exit(1);
                }


                numReplies_.erase(reply.client_seq());
                sendTimes_.erase(reply.client_seq());

                numExecuted_++;
                if (numExecuted_ >= 1000) {
                    exit(0);
                }    

                SubmitRequest();
            }
#endif
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

        sendTimes_[nextReqSeq_] = request.send_time();


#if USE_PROXY && PROTOCOL == DOMBFT
        Address &addr = proxyAddrs_[0];

        // TODO maybe client should own the memory instead of proxy.
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(addr);
        VLOG(1) << "Sent request number " << nextReqSeq_ << " to " << addr.GetIPAsString();

#else
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::CLIENT_REQUEST);
        // TODO check errors for all of these lol
        // TODO do this while waiting, not in the critical path
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);

        for (const Address &addr : replicaAddrs_)
        {
            endpoint_->SendPreparedMsgTo(addr, true);
        }
        endpoint_->setBufReady(false);
#endif


        nextReqSeq_++;

        // TODO record this outstanding request somewhere
    }

} // namespace dombft