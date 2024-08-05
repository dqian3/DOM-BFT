#include "replica.h"

#include "processes/config_util.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <openssl/pem.h>
#include <assert.h>

namespace dombft
{
    using namespace dombft::proto;

    Replica::Replica(const ProcessConfig &config, uint32_t replicaId)
        : replicaId_(replicaId)
        , instance_(0)
    {
        // TODO check for config errors
        std::string replicaIp = config.replicaIps[replicaId];
        LOG(INFO) << "replicaIP=" << replicaIp;

        std::string bindAddress = config.receiverLocal ? "0.0.0.0" : replicaIp;
        LOG(INFO) << "bindAddress=" << bindAddress;

        int replicaPort = config.replicaPort;
        LOG(INFO) << "replicaPort=" << replicaPort;

        std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".pem";
        LOG(INFO) << "Loading key from " << replicaKey;
        if (!sigProvider_.loadPrivateKey(replicaKey))
        {
            LOG(ERROR) << "Unable to load private key!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("client", config.clientKeysDir))
        {
            LOG(ERROR) << "Unable to load client public keys!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("receiver", config.receiverKeysDir))
        {
            LOG(ERROR) << "Unable to load receiver public keys!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("replica", config.replicaKeysDir))
        {
            LOG(ERROR) << "Unable to load receiver public keys!";
            exit(1);
        }


        f_ = config.replicaIps.size() / 3;



        log_ = std::make_unique<Log>();

        if (config.transport == "nng")
        {
            auto addrPairs = getReplicaAddrs(config, replicaId_);
            endpoint_ = std::make_unique<NngEndpoint>(addrPairs, true);

            size_t nClients = config.clientIps.size();
            for (size_t i = 0; i < nClients; i++) {
                // LOG(INFO) << "Client " << i << ": " << addrPairs[i].second.GetIPAsString();
                clientAddrs_.push_back(addrPairs[i].second);
            }

            for (size_t i = nClients + 1; i < addrPairs.size(); i++) {
                LOG(INFO) << "Replica " << i << ": " <<  addrPairs[i].second.GetIPAsString();

                // Skip adding one side of self connection so replica chooses one side to send to
                // TODO this is very ugly.
                if (i - (nClients + 1) == replicaId_) continue;
                replicaAddrs_.push_back(addrPairs[i].second);
            }
        }
        else
        {
            endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);

            /** Store all replica addrs */
            for (uint32_t i = 0; i < config.replicaIps.size(); i++)
            {
                replicaAddrs_.push_back(Address(config.replicaIps[i],
                                                config.replicaPort));
            }

            /** Store all client addrs */
            for (uint32_t i = 0; i < config.clientIps.size(); i++)
            {
                clientAddrs_.push_back(Address(config.clientIps[i],
                                               config.clientPort));
            }
        }


        MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
        {
            this->handleMessage(msgHdr, msgBuffer, sender);
        };

        endpoint_->RegisterMsgHandler(handler);

        fallbackStartTimer_ = std::make_unique<Timer>(
            [this](void *ctx, void *endpoint) 
            {
                endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
                this->startFallback();
            },
            10000,
            this
        );


        fallbackTimer_ = std::make_unique<Timer>(
            [this](void *ctx, void *endpoint) 
            {
                LOG(INFO) << "Fallback for instance=" << instance_ << " failed!";  
                this->startFallback();
            },
            20000,
            this
        );
    }

    Replica::~Replica()
    {
        // TODO cleanup... though we don't really reuse this
    }

    void Replica::run()
    {
        // Submit first request
        LOG(INFO) << "Starting event loop...";
        endpoint_->LoopRun();
    }

#if PROTOCOL == DOMBFT
    void Replica::handleMessage(MessageHeader *hdr, byte *body,
                                Address *sender)
    {
        if (hdr->msgLen < 0)
        {
            return;
        }

#if USE_PROXY

#if FABRIC_CRYPTO
        // TODO find id correctly
        if (!sigProvider_.verify(hdr, body, "receiver", 0))
        {
            LOG(INFO) << "Failed to verify receiver signatures";
            return;
        }
#endif

        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            DOMRequest domHeader;
            ClientRequest clientHeader;

            if (!domHeader.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            // TODO This seems bad...
            // Separate this out into another function probably.
            MessageHeader *clientMsgHdr = (MessageHeader *)domHeader.client_req().c_str();
            byte *clientBody = (byte *)(clientMsgHdr + 1);
            if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

            if (!sigProvider_.verify(clientMsgHdr, clientBody, "client", clientHeader.client_id()))
            {
                LOG(INFO) << "Failed to verify client signature!";
                return;
            }

            // TODO also pass client request
            handleClientRequest(clientHeader);
        }
#else
        if (hdr->msgType == CLIENT_REQUEST)
        {
            ClientRequest clientHeader;

            if (!clientHeader.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

            if (!sigProvider_.verify(hdr, body, "client", clientHeader.client_id()))
            {
                LOG(INFO) << "Failed to verify client signature!";
                return;
            }

            handleClientRequest(clientHeader);
        }
#endif

        if (hdr->msgType == CERT)
        {
            Cert cert;

            if (!cert.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CERT message";
                return;
            }

            handleCert(cert);
        }

        if (hdr->msgType == REPLY)
        {
            Reply replyHeader;

            if (!replyHeader.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            if (!sigProvider_.verify(hdr, body, "replica", replyHeader.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature!";
                return;
            }

            handleReply(replyHeader, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == COMMIT)
        {
            Commit commitMsg;

            if (!commitMsg.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            if (!sigProvider_.verify(hdr, body, "replica", commitMsg.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature!";
                return;
            }

            handleCommit(commitMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == FALLBACK_TRIGGER) {
            FallbackTrigger fallbackTriggerMsg;

            if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
                return;
            }

            // Ignore any messages not for your current instance
            if (fallbackTriggerMsg.instance() != instance_) {
                return;
            }

            if (endpoint_->isTimerRegistered(fallbackStartTimer_.get())) {
                LOG(INFO) << "Received fallback trigger again!";
                return;
            }

            LOG(INFO) << "Received fallback trigger from " << fallbackTriggerMsg.client_id() 
                    << " for cseq=" <<  fallbackTriggerMsg.client_seq() << " and instance="
                    << fallbackTriggerMsg.instance();


            // TODO if attached request has been executed in another view,
            // send result back

            if (fallbackTriggerMsg.has_proof()) {
                // TODO verify proof
                LOG(INFO) << "Fallback trigger has a proof, starting fallback!";

                broadcastToReplicas(fallbackTriggerMsg, FALLBACK_TRIGGER);
                startFallback();
            } else {
                endpoint_->RegisterTimer(fallbackStartTimer_.get());

            }
            
        }
    }

#else // if not DOM_BFT
    void Replica::handleMessage(MessageHeader *hdr, byte *body,
                                Address *sender)
    {
        if (hdr->msgLen < 0)
        {
            return;
        }

        if (hdr->msgType == CLIENT_REQUEST)
        {
            // Only leader should get client requests for now
            if (replicaId_ != 0)
            {
                LOG(ERROR) << "Non leader got CLIENT_REQUEST in dummy PBFT/ZYZZYVA";
                return;
            }

            ClientRequest clientHeader;

            if (!clientHeader.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

            if (!sigProvider_.verify(hdr, body, "client", clientHeader.client_id()))
            {
                LOG(INFO) << "Failed to verify client signature!";
                return;
            }

            DummyProto prePrepare;
            prePrepare.set_client_id(clientHeader.client_id());
            prePrepare.set_client_seq(clientHeader.client_seq());
            prePrepare.set_stage(0);
            prePrepare.set_replica_id(replicaId_);
            prePrepare.set_replica_seq(seq_);

            VLOG(3) << "Leader preprepare " << clientHeader.client_id() << ", " << clientHeader.client_seq() << " at sequence number " << seq_;

            seq_++;

            broadcastToReplicas(prePrepare, MessageType::DUMMY_PROTO);
        }

        if (hdr->msgType == DUMMY_PROTO)
        {
            DummyProto msg;
            if (!msg.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DUMMY_PROTO message";
                return;
            }

            if (!sigProvider_.verify(hdr, body, "replica", msg.replica_id()))
            {
                LOG(INFO) << "Failed to verify replica signature!";
                return;
            }

#if PROTOCOL == ZYZ

            // Handle client request currently just replies back to client,
            // Use that here as a hack lol.
            VLOG(2) << "Replica " << replicaId_
                    << " got preprepare for " << msg.replica_seq();
            ClientRequest dummyReq;
            dummyReq.set_client_id(msg.client_id());
            dummyReq.set_client_seq(msg.client_seq());
            handleClientRequest(dummyReq, msg.replica_seq());

#elif PROTOCOL == PBFT
            std::pair<int, int> key = {msg.client_id(), msg.client_seq()};

            if (msg.stage() == 0)
            {
                VLOG(2) << "Replica " << replicaId_ << " got preprepare for " << msg.replica_seq();
                msg.set_stage(1);
                msg.set_replica_id(replicaId_);
                broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
            }
            else if (msg.stage() == 1)
            {
                prepareCount[key]++;

                VLOG(4) << "Prepare received from " << msg.replica_id() << " now " << prepareCount[key] << " out of " << replicaAddrs_.size() / 3 * 2 + 1;

                if (prepareCount[key] == replicaAddrs_.size() / 3 * 2 + 1)
                {
                    VLOG(2) << "Replica " << replicaId_ << " is prepared on " << msg.replica_seq();

                    msg.set_stage(2);
                    msg.set_replica_id(replicaId_);

                    broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
                }
            }
            else if (msg.stage() == 2)
            {
                commitCount[key]++;

                VLOG(4) << "Commit received from " << msg.replica_id() << " now " << commitCount[key] << " out of " << replicaAddrs_.size() / 3 * 2 + 1;

                if (commitCount[key] == replicaAddrs_.size() / 3 * 2 + 1)
                {
                    VLOG(2) << "Replica " << replicaId_ << " is committed on " << msg.replica_seq();

                    ClientRequest dummyReq;
                    dummyReq.set_client_id(msg.client_id());
                    dummyReq.set_client_seq(msg.client_seq());
                    handleClientRequest(dummyReq, msg.replica_seq());
                }
            }
#endif
        }
    }
#endif

    void Replica::handleClientRequest(const ClientRequest &request)
    {
        Reply reply;
        uint32_t clientId = request.client_id();

        if (clientId < 0 || clientId > clientAddrs_.size())
        {
            LOG(ERROR) << "Invalid client id" << clientId;
            return;
        }

        reply.set_client_id(clientId);
        reply.set_client_seq(request.client_seq());
        reply.set_replica_id(replicaId_);
        reply.set_instance(instance_);

        bool success = log_->addEntry(clientId, request.client_seq(),
                                      (byte *)request.req_data().c_str(),
                                      request.req_data().length());

        if (!success)
        {
            // TODO Handle this more gracefully by queuing requests
            LOG(ERROR) << "Could not add request to log!";
            return;
        }

        uint32_t seq = log_->nextSeq - 1;
        // TODO actually get the result here.
        log_->executeEntry(seq);

        reply.set_fast(true);
        reply.set_seq(seq);

        reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);


        // VLOG(4) << "Start prepare message";
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
        // VLOG(4) << "Finish Serialization, start signature";

        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        // VLOG(4) << "Finish signature";

        LOG(INFO) << "Sending reply back to client " << clientId;
        endpoint_->SendPreparedMsgTo(clientAddrs_[clientId]);

        // Try and commit every 10 replies (half of the way before
        // we can't speculatively execute anymore)
        if (seq % (MAX_SPEC_HIST / 2) == 0)
        {
            LOG(INFO) << "Collecting cert for " << seq << " to checkpoint";

            // TODO remove execution result here
            broadcastToReplicas(reply, MessageType::REPLY);

            if (!log_->tentativeCommitPoint.has_value() || log_->tentativeCommitPoint->seq < seq)
            {
                commitCertReplies.clear();
                commitCertSigs.clear();
                log_->createCommitPoint(seq);
            }
        }
    }

    void Replica::handleCert(const Cert &cert)
    {
        if (!verifyCert(cert))
        {
            return;
        }

        const Reply &r = cert.replies()[0];
        log_->addCert(r.seq(), cert);

        if (log_->lastExecuted < r.seq())
        {
            // Execute up to seq;
        }

        CertReply reply;
        reply.set_client_id(r.client_id());
        reply.set_client_seq(r.client_seq());
        reply.set_replica_id(replicaId_);
 
        VLOG(3) << "Sending cert ack for " << reply.client_id() << ", " << reply.client_seq()
        << " to " << clientAddrs_[reply.client_id()].GetIPAsString();

        // TODO set result
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::CERT_REPLY);
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(clientAddrs_[reply.client_id()]);
    }

    void Replica::handleReply(const dombft::proto::Reply &reply, std::span<byte> sig)
    {
        if (!log_->tentativeCommitPoint.has_value() && reply.seq() >= log_->nextSeq)
        {
            LOG(INFO) << "Received reply for operation replica hasn't processed, adding to new cert";
            commitCertReplies.clear();
            commitCertSigs.clear();
            log_->createCommitPoint(reply.seq());
        }

        if (reply.seq() != log_->tentativeCommitPoint->seq)
        {
            LOG(ERROR) << "Received reply for seq=" << reply.seq()
                       << " not equal to tent. commit point seq="
                       << log_->tentativeCommitPoint->seq;
            return;
        }

        if (log_->tentativeCommitPoint->cert.has_value())
        {
            VLOG(3) << "Ignoring reply from " << reply.replica_id() << " since cert already created";
            return;
        }

        VLOG(3) << "Processing reply from replica " << reply.replica_id() << " for seq " << reply.seq();

        commitCertReplies[reply.replica_id()] = reply;
        commitCertSigs[reply.replica_id()] = std::string(sig.begin(), sig.end());

        std::map<std::tuple<std::string, int, int>, std::set<int>> matchingReplies;

        for (const auto &entry : commitCertReplies)
        {
            int replicaId = entry.first;
            const Reply &reply = entry.second;

            // TODO this is ugly lol
            // We don't need client_seq/client_id, these are already checked.
            // We also don't check the result here, that only needs to happen in the fast path
            std::tuple<std::string, int, int> key = {
                reply.digest(),
                reply.instance(),
                reply.seq()};

            matchingReplies[key].insert(replicaId);

            // Need 2f + 1 and own reply
            if (matchingReplies[key].size() >= 2 * f_ + 1 && commitCertReplies.count(replicaId_))
            {

                log_->tentativeCommitPoint->cert = Cert();
                dombft::proto::Cert &cert = *log_->tentativeCommitPoint->cert;

                for (auto repId : matchingReplies[key])
                {
                    cert.add_signatures(commitCertSigs[repId]);
                    // THis usage is so weird, is protobuf the right tool?
                    (*cert.add_replies()) = commitCertReplies[repId];
                }

                VLOG(1) << "Created cert for request number " << reply.seq();

                memcpy(log_->tentativeCommitPoint->logDigest, log_->getDigest(reply.seq()), SHA256_DIGEST_LENGTH);
                // TODO set digest here
                memset(log_->tentativeCommitPoint->appDigest, 0, SHA256_DIGEST_LENGTH);

                // Broadcast commmit Message
                // TODO get app digest
                dombft::proto::Commit commit;
                commit.set_replica_id(replicaId_);
                commit.set_seq(reply.seq());
                commit.set_log_digest((const char*) log_->tentativeCommitPoint->logDigest, SHA256_DIGEST_LENGTH);
                commit.set_app_digest((const char*) log_->tentativeCommitPoint->appDigest, SHA256_DIGEST_LENGTH);


                broadcastToReplicas(commit, MessageType::COMMIT);
                return;
            }
        }
    }

    void Replica::handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig)
    {
        if (!log_->tentativeCommitPoint.has_value()) {
            // TODO handle this case, we probably would need to request a cert from another replica!
            LOG(ERROR) << "Received COMMIT message without commit point!";
            return;
        }

        VLOG(3) << "Processing COMMIT from " << commitMsg.replica_id() << " for seq " << commitMsg.seq();

        LogCommitPoint &point = *log_->tentativeCommitPoint;

        // Convert to string for comparison
        std::string lDigest(point.logDigest, point.logDigest + SHA256_DIGEST_LENGTH);
        std::string aDigest(point.appDigest, point.appDigest + SHA256_DIGEST_LENGTH);

        // Check message matches commit
        if (commitMsg.seq() != point.seq || commitMsg.log_digest() != lDigest || commitMsg.app_digest() != aDigest)
        {
            LOG(INFO) << "Commit message from " << commitMsg.replica_id() << " does not match current commit!";

            VLOG(3) << "commitMsg.seq=" << commitMsg.seq() << " point.seq=" << point.seq << 
                "\ncommitMsg.log_digest: " << digest_to_hex((const byte *) commitMsg.log_digest().c_str()) << 
                "\n    point.log_digest: " << digest_to_hex(point.logDigest);

            return;
        }


        point.commitMessages[commitMsg.replica_id()] = commitMsg;
        
        if (point.commitMessages.size() >= 2 * f_ + 1) {
            LOG(INFO) << "Committing seq=" << commitMsg.seq();

            log_->commitCommitPoint();
        }
    }

    void Replica::broadcastToReplicas(const google::protobuf::Message &msg, MessageType type)
    {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
        // TODO check errors for all of these lol
        // TODO this sends to self as well, could shortcut this
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        for (const Address &addr : replicaAddrs_)
        {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }

    bool Replica::verifyCert(const Cert &cert)
    {
        if (cert.replies().size() < 2 * f_ + 1)
        {
            LOG(INFO) << "Received cert of size " << cert.replies().size()
                      << ", which is smaller than 2f + 1, f=" << f_;
            return false;
        }

        if (cert.replies().size() != cert.signatures().size())
        {
            LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to "
                      << "cert signatures size" << cert.signatures().size();
            return false;
        }

        // Verify each signature in the cert
        for (int i = 0; i < cert.replies().size(); i++)
        {
            const Reply &reply = cert.replies()[i];
            const std::string &sig = cert.signatures()[i];
            std::string serializedReply = reply.SerializeAsString(); // TODO skip reseraizliation here?

            if (!sigProvider_.verify(
                    (byte *)serializedReply.c_str(),
                    serializedReply.size(),
                    (byte *)sig.c_str(),
                    sig.size(),
                    "replica",
                    reply.replica_id()))
            {
                LOG(INFO) << "Cert failed to verify!";
                return false;
            }
        }

        // TOOD verify that cert actually contains matching replies...
        // And that there aren't signatures from the same replica.

        return true;
    }


    void Replica::handleMessageFallback(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
    {

    }

    void Replica::startFallback()
    {
        instance_++;
        LOG(INFO) << "Starting fallback for instance " << instance_;

        if (endpoint_->isTimerRegistered(fallbackTimer_.get())) {
            endpoint_->ResetTimer(fallbackTimer_.get());
        } else {
            endpoint_->RegisterTimer(fallbackTimer_.get());
        }

        // Extract log into start fallback message
        FallbackStart fallbackStartMsg;
        fallbackStartMsg.set_instance(instance_);
        log_->toProto(&fallbackStartMsg);

        endpoint_->PrepareProtoMsg(fallbackStartMsg, FALLBACK_START);
        endpoint_->SendPreparedMsgTo(replicaAddrs_[instance_ % replicaAddrs_.size()])
    }

    void Replica::finishFallback()
    {

    }


} // namespace dombft