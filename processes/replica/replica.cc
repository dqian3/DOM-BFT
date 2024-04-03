#include "replica.h"

#include <openssl/pem.h>

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

        // TODO find id correctly
        if (!sigProvider_.verify(hdr, body, "receiver", 0))
        {
            LOG(INFO) << "Failed to verify receiver signatures";
            return;
        }

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

            Reply reply;

            uint32_t clientId = clientHeader.client_id();
            reply.set_client_id(clientId);
            reply.set_client_seq(clientHeader.client_seq());
            reply.set_replica_id(replicaConfig_.replicaId);
            reply.set_fast(true);

            // TODO do this for real and actually process the message
            reply.set_view(0);
            reply.set_seq(0);

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
    }
} // namespace dombft