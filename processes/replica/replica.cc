#include "replica.h"

#include <openssl/pem.h>
#include <message_type.h>

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

        std::string replicaIp = replicaConfig_.replicaIp;
        LOG(INFO) << "replicaIP=" << replicaIp;
        int replicaPort = replicaConfig_.replicaPort;
        LOG(INFO) << "replicaPort=" << replicaPort;

        if(!sigProvider_.loadPrivateKey(replicaConfig_.replicaKey)) {
            LOG(ERROR) << "Unable to load private key!";
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

        // TODO verify

        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            DOMRequest domHeader;
            ClientRequest clientHeader;

            if (!domHeader.ParseFromArray(body, hdr->msgLen))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            if (!clientHeader.ParseFromString(domHeader.client_req()))
            {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }


            Reply reply;

            reply.set_client_id(clientHeader.client_id());
            reply.set_client_seq(clientHeader.client_seq());
            reply.set_replica_id(replicaConfig_.replicaId);
            reply.set_fast(true);

            // TODO do this for real
            reply.set_view(0);
            reply.set_seq(0);

            endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
            // sigprovider
            // TODO send to clietn.
            // endpoint_->SendPreparedMsgTo();
        }
    }
} // namespace dombft