#include "replica.h"

#include <openssl/pem.h>
#include <assert.h>

namespace dombft
{
    using namespace dombft::proto;

    Replica::Replica(const std::string &configFile)
    {
        LOG(INFO) << "Loading config information from " << configFile;
        std::string error = replicaConfig_.parseConfig(configFile);
        if (error != "")
        {
            LOG(ERROR) << "Error loading replica config: " << error << " Exiting.";
            exit(1);
        }

        // TODO check for config errors
        std::string replicaIp = replicaConfig_.replicaIps[replicaConfig_.replicaId];
        LOG(INFO) << "replicaIP=" << replicaIp;
        int replicaPort = replicaConfig_.replicaPort;
        LOG(INFO) << "replicaPort=" << replicaPort;

        if (!sigProvider_.loadPrivateKey(replicaConfig_.replicaKey))
        {
            LOG(ERROR) << "Unable to load private key!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("client", replicaConfig_.clientKeysDir))
        {
            LOG(ERROR) << "Unable to load client public keys!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("receiver", replicaConfig_.receiverKeysDir))
        {
            LOG(ERROR) << "Unable to load receiver public keys!";
            exit(1);
        }

        if (!sigProvider_.loadPublicKeys("replica", replicaConfig_.replicaKeysDir))
        {
            LOG(ERROR) << "Unable to load receiver public keys!";
            exit(1);
        }


        /** Store all replica addrs */
        for (uint32_t i = 0; i < replicaConfig_.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(replicaConfig_.replicaIps[i],
                                            replicaConfig_.replicaPort));
        }

        endpoint_ = new UDPEndpoint(replicaIp, replicaPort, true);
        handler_ = new UDPMessageHandler(
            [](MessageHeader *msgHdr, byte *msgBuffer, Address *sender, void *ctx)
            {
                ((Replica *)ctx)->handleMessage(msgHdr, msgBuffer, sender);
            },
            this);

        endpoint_->RegisterMsgHandler(handler_);
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
            MessageHeader *clientMsgHdr = (MessageHeader *) domHeader.client_req().c_str();
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
            handleClientRequest(clientHeader, seq_);
            seq_++;
       }
#elif PROTOCOL == DOMBFT
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

#else // TODO do separate replica processes instead of these compile flags lol
        if (hdr->msgType == CLIENT_REQUEST)
        {
            // Only leader should get client requests for now
            if (replicaConfig_.replicaId != 0) {
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
            prePrepare.set_replica_id(replicaConfig_.replicaId);
            prePrepare.set_replica_seq(seq_);

            VLOG(3) << "Leader preprepare " <<  clientHeader.client_id() << ", " <<
                    clientHeader.client_seq() << " at sequence number " << seq_;

            seq_++;

            broadcastToReplicas(prePrepare, MessageType::DUMMY_PROTO);
        }

        if (hdr->msgType == DUMMY_PROTO) {
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
            VLOG(2) << "Replica " << replicaConfig_.replicaId 
                    << " got preprepare for " << msg.replica_seq();
            ClientRequest dummyReq;
            dummyReq.set_client_id(msg.client_id());
            dummyReq.set_client_seq(msg.client_seq());
            handleClientRequest(dummyReq, msg.replica_seq());

#elif PROTOCOL == PBFT
            std::pair<int, int> key = {msg.client_id(), msg.client_seq()};

            if (msg.stage() == 0) {
                VLOG(2) << "Replica " << replicaConfig_.replicaId << " got preprepare for " << msg.replica_seq();
                msg.set_stage(1);
                msg.set_replica_id(replicaConfig_.replicaId);
                broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
            } else if (msg.stage() == 1) {
                prepareCount[key]++;

                VLOG(4) << "Prepare received from " << msg.replica_id() << " now " << prepareCount[key] << " out of " << replicaAddrs_.size() / 3 * 2 + 1;

                if (prepareCount[key] == replicaAddrs_.size() / 3 * 2 + 1) {
                    VLOG(2) << "Replica " << replicaConfig_.replicaId << " is prepared on " << msg.replica_seq();

                    msg.set_stage(2);
                    msg.set_replica_id(replicaConfig_.replicaId);

                    broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
                }
            } else if (msg.stage() == 2) {
                commitCount[key]++;

                VLOG(4) << "Commit received from " << msg.replica_id() << " now " << commitCount[key] << " out of " << replicaAddrs_.size() / 3 * 2 + 1;



                if (commitCount[key] == replicaAddrs_.size() / 3 * 2 + 1) {
                    VLOG(2) << "Replica " << replicaConfig_.replicaId << " is committed on " << msg.replica_seq();

                    ClientRequest dummyReq;
                    dummyReq.set_client_id(msg.client_id());
                    dummyReq.set_client_seq(msg.client_seq());
                    handleClientRequest(dummyReq, msg.replica_seq());
                }
            }

#endif
            
        }
#endif
    }

    void Replica::handleClientRequest(const ClientRequest &request, uint32_t seq)
    {
        Reply reply;

        uint32_t clientId = request.client_id();
        reply.set_client_id(clientId);
        reply.set_client_seq(request.client_seq());
        reply.set_replica_id(replicaConfig_.replicaId);
        reply.set_fast(true);

        // TODO do this for real and actually process the message
        reply.set_view(0);
        reply.set_seq(seq);

        MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        // TODO send to client

        if (clientId < 0 || clientId > replicaConfig_.clientIps.size())
        {
            LOG(ERROR) << "Invalid client id" << clientId;
            return;
        }

        LOG(INFO) << "Sending reply back to client " << clientId;
        endpoint_->SendPreparedMsgTo(Address(replicaConfig_.clientIps[clientId], replicaConfig_.clientPort));
    }

    void Replica::broadcastToReplicas(const google::protobuf::Message &msg, MessageType type) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
        // TODO check errors for all of these lol
        // TODO do this while waiting, not in the critical path
        sigProvider_.appendSignature(hdr, UDP_BUFFER_SIZE);
        for (const Address &addr : replicaAddrs_)
        {
            endpoint_->SendPreparedMsgTo(addr, true);
        }
        endpoint_->setBufReady(false);
    }

} // namespace dombft