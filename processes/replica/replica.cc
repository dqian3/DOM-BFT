#include "replica.h"

#include "processes/config_util.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"

// #include "lib/apps/kv_rocksdb.h"

#include <openssl/pem.h>
#include <assert.h>

namespace dombft
{
    using namespace dombft::proto;

    Replica::Replica(const ProcessConfig &config, uint32_t replicaId)
        : replicaId_(replicaId), replicaState_(ReplicaState::FAST_PATH), biggestDeadline_(0)
    {
        // TODO check for config errors
        std::string replicaIp = config.replicaIps[replicaId];
        LOG(INFO) << "replicaIP=" << replicaIp;

        std::string bindAddress = config.receiverLocal ? "0.0.0.0" : replicaIp;
        LOG(INFO) << "bindAddress=" << bindAddress;

        int replicaPort = config.replicaPort;
        LOG(INFO) << "replicaPort=" << replicaPort;

        std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".pem";

        forceNormal_ = config.forceNormal;

        LOG(INFO) << "forceNormal=" << forceNormal_;

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

        /** Store all replica addrs */
        for (uint32_t i = 0; i < config.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(config.replicaIps[i],
                                            config.replicaPort));
        }
        f_ = replicaAddrs_.size() / 3;

        /** Store all client addrs */
        for (uint32_t i = 0; i < config.clientIps.size(); i++)
        {
            clientAddrs_.push_back(Address(config.clientIps[i],
                                           config.clientPort));
        }

        log_ = std::make_unique<Log>();

        inMemoryDB_.Open();

        if (config.transport == "nng") {
            auto addrPairs = getReplicaAddrs(config, replicaId_);
            endpoint_ = std::make_unique<NngEndpoint>(addrPairs, true);
        } else {
            endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);

        }
        MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
        {
            this->handleMessage(msgHdr, msgBuffer, sender);
        };

        endpoint_->RegisterMsgHandler(handler);
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

    void Replica::setState(ReplicaState newState)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        replicaState_ = newState;
    }

    Replica::ReplicaState Replica::getState()
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return replicaState_;
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

            LOG(INFO) << "Received a request with deadline " << domHeader.deadline();

            // LOG(INFO) << "This amount of time has elpased from the moment when the request is generated: " << GetMicrosecondTimestamp() -  domHeader.send_time();

            updateDeadline(domHeader.deadline());
            // TODO also pass client request
            handleClientRequest(clientHeader);

            LOG(INFO) << "Done Sending reply back to receiver";
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

    // the way a replica handles the client request is dependent on the current state of the replica. 
    void Replica::handleClientRequest(const ClientRequest &request)
    {
        Reply reply;
        uint32_t clientId = request.client_id();

        std::string request_data(request.req_data().begin(), request.req_data().end());
        LOG(INFO) << "Received request from client " << clientId << " with key " << request_data;

        std::istringstream iss(request_data);
        std::string op, key, value;

        iss >> op >> key >> value;
        LOG(INFO) << "Operation: " << op << " Key: " << key << " Value: " << value;

        if (needToForceNormal())
        {
            setState(ReplicaState::NORMAL_PATH);
            LOG(INFO) << "Forcing normal path";
        }

        if (this->getState() == ReplicaState::FAST_PATH)
        {   
            LOG(INFO) << "fast path taken";
            // db_.beginTransaction();
            // db_.set(key, value);
            // db_.commit();

            // actually update the database at the replica instead of at the log.
            inMemoryDB_.Execute(DB_STORE::UNSTABLE, request_data, request_data.length());

            if (clientId < 0 || clientId > clientAddrs_.size())
            {
                LOG(ERROR) << "Invalid client id" << clientId;
                return;
            }

            reply.set_client_id(clientId);
            reply.set_client_seq(request.client_seq());
            reply.set_replica_id(replicaId_);

            // TODO change this when we implement the slow path
            reply.set_view(0);

            bool fast = log_->addEntry(clientId, request.client_seq(),
                                    (byte *)request.req_data().c_str(),
                                    request.req_data().length());

            reply.set_fast(fast);
            reply.set_seq(log_->nextSeq - 1);

            reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::FAST_REPLY);
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);

            LOG(INFO) << "Sending reply back to client " << clientId;
            endpoint_->SendPreparedMsgTo(clientAddrs_[clientId]);
            LOG(INFO) << "Endpoint prepared Message Sent!";
            LOG(INFO) << "This amount of time has elpased from the moment when the request is generated: " << GetMicrosecondTimestamp() -  request.send_time();

        }
        else if (this->getState() == ReplicaState::NORMAL_PATH)
        {

            bool normal = log_->addEntry(clientId, request.client_seq(),
                                    (byte *)request.req_data().c_str(),
                                    request.req_data().length());

            reply.set_client_id(clientId);
            reply.set_client_seq(request.client_seq());
            reply.set_replica_id(replicaId_);

            reply.set_view(0);
            reply.set_fast(false);
            reply.set_seq(log_->nextSeq - 1);

            reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
            sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
            endpoint_->SendPreparedMsgTo(clientAddrs_[clientId]);


            LOG(WARNING) << "forced a normal path";
            LOG(INFO) << "Replica is in normal path, which has not been implemented yet";
            setState(ReplicaState::FAST_PATH);

        }
        else
        {
            LOG(INFO) << "Replica is in slow path";
        }

    }

    bool Replica::needToForceNormal()
    { 
        // one case would be when the the next seq in the log indicate that normal path has not been taken for a while
        if (log_->nextSeq % forceNormal_ == 0)
        {
            return true;
        }
        return false;
    }

    void Replica::handleCert(const Cert &cert)
    {

        // once we receive a cert, we know that either there is something wrong, or client trying to force us to take a normal path
        // to commit the current requests. 
        // Either way, we need to revert to the normal path.
        // setState(ReplicaState::NORMAL_PATH);
        // LOG(INFO) << "Received a cert, reverting to normal path";

        // TODO verify cert, for now just accept it!
        if (cert.replies().size() < 2 * f_ + 1)
        {
            LOG(INFO) << "Received cert of size " << cert.replies().size()
                      << ", which is smaller than 2f + 1, f=" << f_;
            return;
        }

        if (cert.replies().size() != cert.signatures().size())
        {
            LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to "
                      << "cert signatures size" << cert.signatures().size();
            return;
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
                return;
            }
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

        VLOG(3) << "Received cert for " << reply.client_id() << ", " << reply.client_seq();

        // TODO set result
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::CERT_REPLY);
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(clientAddrs_[reply.client_id()]);

        LOG(INFO) << "send cert reply to client: " << reply.client_id() << "IP: " << clientAddrs_[reply.client_id()].GetIPAsString();
    }

    void Replica::broadcastToReplicas(const google::protobuf::Message &msg, MessageType type)
    {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
        // TODO check errors for all of these lol
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        for (const Address &addr : replicaAddrs_)
        {
            endpoint_->SendPreparedMsgTo(addr);
        }
    }

    void Replica::updateDeadline(uint64_t deadline)
    {
        std::lock_guard<std::mutex> lock(deadlineMutex_);
        if (deadline > biggestDeadline_)
        {
            LOG(INFO) << "updated the deadline to " << deadline;
            biggestDeadline_ = deadline;
        } else {
            // if we receive a deadline that is smaller than the biggest deadline we have seen so far, we need to revert to the normal path
            setState(ReplicaState::NORMAL_PATH);
            LOG(INFO) << "Received a smaller deadline, reverting to normal path";
        }
    }

} // namespace dombft