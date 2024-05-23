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

        f_ = replicaAddrs_.size() / 3;

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

        endpoint_ = std::make_unique<UDPEndpoint>(clientIP, clientPort, true);
        replyHandler_ = std::make_unique<UDPMessageHandler>(
            [](MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Client *)ctx)->receiveReply(msgHdr, msgBuffer, sender);
            },
            this);

        // Handler only lives as long as the parent class
        endpoint_->RegisterMsgHandler(replyHandler_.get());

        timeoutTimer_ = std::make_unique<Timer>(
            [](void *ctx, void *endpoint)
            {
                ((Client *)ctx)->checkTimeouts();
            },
            5000,
            this);

        endpoint_->RegisterTimer(timeoutTimer_.get());
    }

    Client::~Client()
    {
        // TODO cleanup... though we don't really reuse this
    }

    void Client::Run()
    {
        // Submit first request
        for (uint32_t i = 0; i < maxInFlight_; i++)
        {
            submitRequest();
        }
        endpoint_->LoopRun();
    }

    void Client::receiveReply(MessageHeader *msgHdr, byte *msgBuffer,
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

            if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", reply.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature!";
                return;
            }

            // Check validity
            //  1. Not for the same client
            //  2. Client doesn't have state for this request
            if (reply.client_id() != clientId_ || reply.client_seq())
            {
                // TODO more info?
                LOG(INFO) << "Invalid reply!";
                return;
            }

            auto &reqState = requestStates_[reply.client_seq()];

            VLOG(4) << "Received reply from replica " << reply.replica_id()
                    << " after " << GetMicrosecondTimestamp() - reqState.sendTime << " usec";

            // TODO handle dups
            reqState.replies[reply.replica_id()] = reply;
            reqState.signatures[reply.replica_id()] = sigProvider_.getSignature(msgHdr, msgBuffer);

#if PROTOCOL == PBFT
            if (numReplies_[reply.client_seq()] >= clientConfig_.replicaIps.size() / 3 * 2 + 1)
            {

                LOG(INFO) << "PBFT commit for " << reply.client_seq() - 1 << " took "
                          << GetMicrosecondTimestamp() - sendTimes_[reply.client_seq()] << " usec";

                numReplies_.erase(reply.client_seq());
                sendTimes_.erase(reply.client_seq());

                numExecuted_++;
                if (numExecuted_ >= 1000)
                {
                    exit(0);
                }

                SubmitRequest();
            }

#else
            checkReqState(reply.client_seq());
#endif
        }

        else if (msgHdr->msgType == MessageType::CERT_REPLY) {
            CertReply certReply;

            if (!certReply.ParseFromArray(msgBuffer, msgHdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CERT_REPLY message";
                return;
            }


            if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", certReply.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature for CERT_REPLY!";
                return;
            }

            auto &reqState = requestStates_[certReply.client_seq()];

            reqState.certReplies.insert(certReply.replica_id());

            if (reqState.certReplies.size() >= 2 * f_ + 1) {
                // Request is committed, so we can clean up state!
                requestStates_.erase(certReply.client_seq());
                submitRequest();
            }

        }
    }

    void Client::submitRequest()
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

        requestStates_[nextReqSeq_].certTime = request.send_time();

#if USE_PROXY && PROTOCOL == DOMBFT
        Address &addr = proxyAddrs_[0];

        // TODO maybe client should own the memory instead of endpoint.
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
    }

    void Client::checkTimeouts()
    {
        uint64_t now = GetMicrosecondTimestamp();

        for (auto& entry: requestStates_) {
            int clientSeq = entry.first;
            const RequestState &reqState = entry.second;

            if (now - reqState.certTime > NORMAL_PATH_TIMEOUT) {
                // Send cert to replicas;
                assert(reqState.cert.has_value());

                endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);
                for (const Address &addr : replicaAddrs_)
                {
                    endpoint_->SendPreparedMsgTo(addr, true);
                }            
                endpoint_->setBufReady(false);
            } 
            if (now - reqState.sendTime > RETRY_TIMEOUT){
                LOG(ERROR) << "Client failed on request " << clientSeq;
                exit(1);
            }
        }
    }

    void Client::checkReqState(uint32_t client_seq)
    {
        auto &reqState = requestStates_[client_seq];

        if (reqState.cert.has_value() && reqState.replies.size() == 3 * f_ + 1)
        {
            // We already have a cert, so fast path might be possible.
            // TODO check if fast path is not posssible and prevent extra work
            // TODO this fast path is quite basic, since it just checks for 3 f + 1 fast replies
            // we want it to do a bit more, specifically
            // 1. Check if we can still take the fast path if there are some normal replies
            // 2. Send certs to replicas with normal path to catch them up

            auto it = reqState.replies.begin();
            std::optional<std::string> result;

            const Reply &rep1 = it->second;
            if (!rep1.fast())
            {
                // This means all responses need to be fast
                return;
            }

            it++;
            for (; it != reqState.replies.end(); it++)
            {
                const Reply &rep2 = it->second;
                if (rep1.view() != rep2.view() || rep1.seq() != rep2.seq() || rep1.digest() != rep2.digest() || rep1.result() != rep2.result())
                    return;
            }

            // TODO Deliver to application
            // Request is committed and can be cleaned up.

            requestStates_.erase(client_seq);
            submitRequest();
        }
        else
        {
            // Try and find a certificate
            std::map<std::tuple<std::string, int, int>, std::set<int>> matchingReplies;

            for (const auto &entry : reqState.replies)
            {
                int replicaId = entry.first;
                const Reply &reply = entry.second;

                // TODO this is ugly lol
                // We don't need client_seq/client_id, these are already checked.
                // We also don't check the result here, that only needs to happen in the fast path
                std::tuple<std::string, int, int> key = {
                    reply.digest(),
                    reply.view(),
                    reply.seq()};

                matchingReplies[key].insert(replicaId);

                if (matchingReplies[key].size() >= 2 * f_ + 1)
                {
                    reqState.cert = Cert();

                    // TODO check if fast path is not posssible, and we can send cert right away
                    for (auto repId : matchingReplies[key])
                    {
                        reqState.cert->add_signatures(reqState.signatures[repId]);
                        // THis usage is so weird, is protobuf the right tool?
                        (*reqState.cert->add_replies()) = reqState.replies[repId];
                    }

                    reqState.certTime = GetMicrosecondTimestamp();
                    return;
                }
            }
        }
    }

} // namespace dombft