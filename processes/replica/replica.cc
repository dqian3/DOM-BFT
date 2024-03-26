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

        std::string replicaIp = replicaConfig_.replicaIp;
        LOG(INFO) << "replicaIP=" << replicaIp;
        int replicaPort = replicaConfig_.replicaPort;
        LOG(INFO) << "replicaPort=" << replicaPort;

        BIO *bo = BIO_new_file(replicaConfig_.replicaKey.c_str(), "r");
        EVP_PKEY *key = NULL;
        PEM_read_bio_PrivateKey(bo, &key, 0, 0);
        BIO_free(bo);

        if (key == NULL)
        {
            LOG(ERROR) << "Unable to load replica private key!";
            exit(1);
        }

        /** Store all replica addrs */
        for (uint32_t i = 0; i < replicaConfig_.replicaIps.size(); i++)
        {
            replicaAddrs_.push_back(Address(replicaConfig_.replicaIps[i],
                                            replicaConfig_.replicaPort));
        }

        endpoint_ = new SignedUDPEndpoint(replicaIp, replicaPort, key, true);
        replyHandler_ = new UDPMsgHandler(
            [](MessageHeader *msgHdr, char *msgBuffer, Address *sender, void *ctx)
            {
                ((Replica *)ctx)->ReceiveRequest(msgHdr, msgBuffer, sender);
            },
            this);

        endpoint_->RegisterMsgHandler(replyHandler_);
    }

    Replica::~Replica()
    {
        // TODO cleanup... though we don't really reuse this
    }

    void Replica::Run()
    {
        // Submit first request
        LOG(INFO) << "Starting event loop...";
        endpoint_->LoopRun();
    }

    void Replica::ReceiveRequest(MessageHeader *hdr, char *body,
                                  Address *sender)
    {
        if (hdr->msgLen < 0)
        {
            return;
        }
        DOMRequest request;
        if (hdr->msgType == MessageType::DOM_REQUEST)
        {
            SignedMessageHeader *shdr = (SignedMessageHeader *) body;
            u_char *reqBytes = (u_char *)(shdr + 1);

            // TODO verify and handle signed header better
            if (!request.ParseFromArray(reqBytes, hdr->msgLen - shdr->sigLen - sizeof(SignedMessageHeader)))
            {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            // Send measurement reply right away
            uint64_t recv_time = GetMicrosecondTimestamp();

            MeasurementReply mReply;
            mReply.set_replica_id(replicaConfig_.replicaId);
            mReply.set_owd(recv_time - request.send_time());
            VLOG(3) << "Measured delay: " << mReply.owd();

            // TODO get address better lol
            endpoint_->SignAndSendProtoMsgTo(Address(sender->GetIPAsString(), replicaConfig_.proxyMeasurementPort), mReply, MessageType::MEASUREMENT_REPLY);

            // Check if request is on time.
            request.set_late(recv_time < request.deadline());

            // Forward request

            if (replicaConfig_.ipcReplica)
            {
                // TODO
                throw "IPC communciation not implemented";
            }
            else
            {
                VLOG(1) << "Forwarding Request with deadline " << request.deadline();

                for (const Address &addr : replicaAddrs_)
                {
                    endpoint_->SignAndSendProtoMsgTo(addr, request, MessageType::DOM_REQUEST);
                }
            }
        }
    }

} // namespace dombft