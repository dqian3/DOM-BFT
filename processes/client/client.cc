#include "client.h"

#include "processes/config_util.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#define NUM_CLIENTS 100

namespace dombft
{
    using namespace dombft::proto;

    Client::Client(const ProcessConfig &config, size_t id)
        : clientId_(id)
    {
        LOG(INFO) << "clientId=" << clientId_;
        std::string clientIp = config.clientIps[clientId_];
        LOG(INFO) << "clientIp=" << clientIp;
        int clientPort = config.clientPort;
        LOG(INFO) << "clientPort=" << clientPort;

        /** Store all proxy addrs. TODO handle mutliple proxy sockets*/
        for (uint32_t i = 0; i < config.proxyIps.size(); i++)
        {
            LOG(INFO) << "Proxy " << i + 1 << ": " << config.proxyIps[i] << ", " << config.proxyForwardPort;
            proxyAddrs_.push_back(Address(config.proxyIps[i],
                                          config.proxyForwardPort));
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < config.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(config.replicaIps[i],
                                            config.replicaPort));
        }
        f_ = replicaAddrs_.size() / 3;

        /* Setup keys */
        std::string clientKey = config.clientKeysDir + "/client" + std::to_string(clientId_) + ".pem";
        LOG(INFO) << "Loading key from " << clientKey;
        if (!sigProvider_.loadPrivateKey(clientKey))
        {
            LOG(ERROR) << "Error loading client private key, exiting...";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("replica", config.replicaKeysDir))
        {
            LOG(ERROR) << "Error loading replica public keys, exiting...";
            exit(1);
        }

        // TODO make this some sort of config
        LOG(INFO) << "Simulating " << config.clientMaxRequests << " simultaneous clients!";
        maxInFlight_ = config.clientMaxRequests;


        /** Initialize state */
        nextReqSeq_ = 1;

        if (config.transport == "nng") {
            auto addrPairs = getClientAddrs(config, clientId_);
            endpoint_ = std::make_unique<NngEndpoint>(addrPairs, true);
        }
        else {
            endpoint_ = std::make_unique<UDPEndpoint>(clientIp, clientPort, true);
        }
        
        
        MessageHandlerFunc replyHandler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
        {
            this->receiveReply(msgHdr, msgBuffer, sender);
        };

        endpoint_->RegisterMsgHandler(replyHandler);

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

    void Client::run()
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
            if (reply.client_id() != clientId_ || requestStates_.count(reply.client_seq()) == 0)
            {
                // TODO more info?
                LOG(INFO) << "Invalid reply!";
                return;
            }

            if (requestStates_.count(reply.client_seq()) == 0)
            {
                VLOG(2) << "Received reply for " << reply.client_seq() << " not in active requests";
                return;
            }


            auto &reqState = requestStates_[reply.client_seq()];

            VLOG(4) << "Received reply from replica " << reply.replica_id()
                    << " for " << reply.client_seq() << " at log pos " << reply.seq()
                    << " after " << GetMicrosecondTimestamp() - reqState.sendTime << " usec";

            // TODO handle dups
            reqState.replies[reply.replica_id()] = reply;
            reqState.signatures[reply.replica_id()] = sigProvider_.getSignature(msgHdr, msgBuffer);

#if PROTOCOL == PBFT
            if (numReplies_[reply.client_seq()] >= f_ / 3 * 2 + 1)
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

            if (msgHdr->msgType == MessageType::REPLY) {
                LOG(INFO) << "Received normal reply for " << reply.client_seq();
                checkReqStateNormal(reply.client_seq(), reply.replica_id());
            } else {
                LOG(INFO) << "Received fast reply for " << reply.client_seq();
                checkReqState(reply.client_seq());
            }
#endif
        }

        else if (msgHdr->msgType == MessageType::CERT_REPLY)
        {
            CertReply certReply;
            // LOG(INFO) << "cert reply received";

            if (!certReply.ParseFromArray(msgBuffer, msgHdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CERT_REPLY message";
                return;
            }


            uint32_t cseq = certReply.client_seq();

            LOG(INFO) << "received a cert reply for: "<< cseq;

            if (!sigProvider_.verify(msgHdr, msgBuffer, "replica", certReply.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature for CERT_REPLY!";
                return;
            }

            if (reply.client_id() != clientId_) {
                VLOG(2) << "Received certReply for client " << reply.client_id()  << " != " << clientId_;
                return;
 
            }

            if (requestStates_.count(cseq) == 0)
            {
                VLOG(2) << "Received certReply for " << cseq << " not in active requests";
                return;
            }
            
            auto &reqState = requestStates_[cseq];

            reqState.certReplies.insert(certReply.replica_id());

            if (reqState.certReplies.size() >= 2 * f_ + 1)
            {
                // Request is committed, so we can clean up state!

                VLOG(1) << "Request " << cseq << " normal path committed! "
                        << "Took " << GetMicrosecondTimestamp() - requestStates_[cseq].sendTime << " us";

                requestStates_.erase(cseq);
                submitRequest();
            }
        }
    }

    void Client::submitRequest()
    {
        ClientRequest request;

        // submit new request
        request.set_client_id(clientId_);
        request.set_client_seq(nextReqSeq_);
        request.set_send_time(GetMicrosecondTimestamp());
        request.set_is_write(true); // TODO modify this based on some random chance

        // generate random set request
        request.set_req_data("SET NYU 65");

        requestStates_[nextReqSeq_].sendTime = request.send_time();

#if USE_PROXY && PROTOCOL == DOMBFT
        // TODO how to chhoose proxy
        Address &addr = proxyAddrs_[clientId_ % proxyAddrs_.size()];

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
            endpoint_->SendPreparedMsgTo(addr);
        }
#endif


        // if (nextReqSeq_ == 1000 + maxInFlight_) {
        //     LOG(INFO) << "Exiting after sending " << 1000 + maxInFlight_  << " requests";
        //     exit(0);
        // }
        nextReqSeq_++;
    }

    void Client::checkTimeouts()
    {
        uint64_t now = GetMicrosecondTimestamp();

        for (auto &entry : requestStates_)
        {
            int clientSeq = entry.first;
            RequestState &reqState = entry.second;

            if (reqState.cert.has_value() && now - reqState.certTime > NORMAL_PATH_TIMEOUT && !reqState.certSent)
            {
                VLOG(1) << "Request number " << clientSeq << " fast path timed out! Sending cert!";

                // Send cert to replicas;
                endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);
                for (const Address &addr : replicaAddrs_)
                {
                    endpoint_->SendPreparedMsgTo(addr);
                }

                reqState.certTime = now; // timeout again later
            }
            if (now - reqState.sendTime > RETRY_TIMEOUT)
            {
                LOG(ERROR) << "Client failed on request " << clientSeq << " sendTime=" << reqState.sendTime << " now=" << now;
                exit(1);
            }
        }
    }

    void Client::checkReqStateNormal(uint32_t clientSeq, uint32_t replicaId) 
    {
        // if we already got 2f+1 certificate for the same position, send back a reply
        auto &reqState = requestStates_[clientSeq];
        // there is a normal path reply, fast path not possible
        reqState.fastPathPossible = false;
        if (reqState.cert.has_value() && !reqState.certSent) {
            // for this normal path reply, a cert has already been generated, send the cert directly to the replicas that is in a normal path

            VLOG(1) << "Sending cert immediately for request number " << clientSeq << " ";
            endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);

            // this is a normal reply, fast path is not possible anymore
            // send out the certificate to all the replicas immediately
            // OR we can selectively send to all those who chose normal path, or whose fast path choice is not correct
            for (const Address &addr : replicaAddrs_)
            {
                endpoint_->SendPreparedMsgTo(addr);
            }

            reqState.certSent = true;
            // reqState.certTime = GetMicrosecondTimestamp(); // timeout again later

            return;
        }
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
                
                VLOG(1) << "Created cert for request number " << clientSeq;
            }
        }

        if (reqState.cert.has_value() && !reqState.certSent) {
            // Send cert to replicas;

            LOG(INFO) << "Normal path collected enough cert, sending cert to replicas!";
            endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);
            for (const Address &addr : replicaAddrs_)
            {
                endpoint_->SendPreparedMsgTo(addr);
            }

            reqState.certSent = true;
            reqState.certTime = GetMicrosecondTimestamp(); // timeout again later
        }

    }

    void Client::checkReqState(uint32_t clientSeq)
    {
        auto &reqState = requestStates_[clientSeq];

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

            VLOG(1) << "Request " << rep1.client_seq() << " fast path committed at global seq " << rep1.seq()
                    << ". Took " << GetMicrosecondTimestamp() - requestStates_[rep1.client_seq()].sendTime << " us";

            requestStates_.erase(clientSeq);
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
                    
                    VLOG(1) << "Created cert for request number " << clientSeq;
#if IMMEDIATE_CERT

                    // if ( GetMicrosecondTimestamp() - reqState.certTime < NORMAL_PATH_TIMEOUT) {
                    //     return;
                    // }
                    
                    VLOG(1) << "Sending cert immediately for request number " << clientSeq << " ";

                    // Send cert to replicas;
                    endpoint_->PrepareProtoMsg(reqState.cert.value(), CERT);
                    for (const Address &addr : replicaAddrs_)
                    {
                        endpoint_->SendPreparedMsgTo(addr);
                    }

                    reqState.certTime = GetMicrosecondTimestamp(); // timeout again later

#endif
                    
                    return;

                }
            }
        }
    }

} // namespace dombft