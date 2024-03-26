#include "proxy.h"

#include <openssl/pem.h>

#include "lib/message_type.h"

namespace dombft
{
    using namespace dombft::proto;

    Proxy::Proxy(const std::string &configFile)
    {
        std::string error = proxyConfig_.parseConfig(configFile);
        if (error != "")
        {
            LOG(ERROR) << "Error parsing proxy config: " << error << "Exiting.";
            exit(1);
        }
        running_ = true;
        
        int numShards = proxyConfig_.proxyNumShards;
        latencyBound_ = proxyConfig_.initialOwd;
        maxOWD_ = proxyConfig_.maxOwd;

        logMap_.resize(numShards);

        BIO *bo = BIO_new_file(proxyConfig_.proxyKey.c_str(), "r");
        EVP_PKEY *key = NULL;
        PEM_read_bio_PrivateKey(bo, &key, 0, 0 );
        BIO_free(bo);

        if(key == NULL) {
            LOG(ERROR) << "Unable to load private key!";
            exit(1);
        }

        for (int i = 0; i < numShards; i++)
        {
            forwardEps_.push_back(new SignedUDPEndpoint(proxyConfig_.proxyIp,
                                                        proxyConfig_.proxyForwardPortBase + i,
                                                        key, false));
        }
        measurmentEp_ = new UDPEndpoint(
            proxyConfig_.proxyIp, proxyConfig_.proxyMeasurementPort);

        numReceivers_ = proxyConfig_.receiverIps.size();
        for (int i = 0; i < numReceivers_; i++)
        {
            std::string receiverIp = proxyConfig_.receiverIps[i];
            receiverAddrs_.push_back(Address(receiverIp, proxyConfig_.receiverPort));
        }
    }

    void Proxy::Terminate()
    {
        LOG(INFO) << "Terminating...";
        running_ = false;
    }

    void Proxy::Run()
    {
        running_ = true;
        
        LaunchThreads();
        for (auto &kv : threads_)
        {
            LOG(INFO) << "Join " << kv.first;
            kv.second->join();
            LOG(INFO) << "Join Complete " << kv.first;
        }
        LOG(INFO) << "Run Terminated ";
    }

    Proxy::~Proxy()
    {
        for (auto &kv : threads_)
        {
            delete kv.second;
        }

        // TODO Cleanup more

    }

    void Proxy::LaunchThreads()
    {
        int shardNum = proxyConfig_.proxyNumShards;

        threads_["RecvMeasurementsTd"] = new std::thread(&Proxy::RecvMeasurementsTd, this);

        for (int i = 0; i < shardNum; i++)
        {
            std::string key = "ForwardRequestsTd-" + std::to_string(i);
            threads_[key] = new std::thread(&Proxy::ForwardRequestsTd, this, i);
        }
    }

    void Proxy::RecvMeasurementsTd()
    {
        std::vector<uint32_t> receieverOWDs(numReceivers_);

        MessageHandlerFunc handleMeasurementReply = [this, &receieverOWDs](MessageHeader *hdr, void *body, Address *sender, void *context)
        {
            MeasurementReply reply;
            SignedMessageHeader *shdr = (SignedMessageHeader *) body;
            u_char *reqBytes = (u_char *)(shdr + 1);

            // TODO verify and handle signed header better
            if (!reply.ParseFromArray(reqBytes, hdr->msgLen - shdr->sigLen - sizeof(SignedMessageHeader)))
            {
                LOG(ERROR) << "Unable to parse Measurement_Reply message";
                return;
            }
            VLOG(1) << "replica=" << reply.receiver_id() << "\towd=" << reply.owd(); 

            if (reply.owd() > 0)
            {
                // TODO actually calculate something here?
                receieverOWDs[reply.receiver_id()] = reply.owd();
                // Update latency bound
                uint32_t estimatedOWD = 0;
                for (uint32_t i = 0; i < receieverOWDs.size(); i++)
                {
                    if (estimatedOWD < receieverOWDs[i])
                    {
                        estimatedOWD = receieverOWDs[i];
                    }
                }
                if (estimatedOWD > maxOWD_)
                {
                    estimatedOWD = maxOWD_;
                }
                VLOG(1) << "Update bound " << latencyBound_ << " => " << estimatedOWD;
                latencyBound_.store(estimatedOWD);
             }
        };

        /* Checks every 10ms to see if we are done*/
        auto checkEnd = [](void *ctx, void *receiverEP)
        {
            if (((Proxy *)ctx)->running_ == false)
            {
                ((Endpoint *)receiverEP)->LoopBreak();
            }
        };

        UDPMessageHandler handler(handleMeasurementReply);
        Timer monitor(checkEnd, 10, this);

        measurmentEp_->RegisterMsgHandler(&handler);
        measurmentEp_->RegisterTimer(&monitor);

        measurmentEp_->LoopRun();
    }

    void Proxy::ForwardRequestsTd(const int thread_id)
    {
        MessageHandlerFunc handleClientRequest = [this, thread_id](MessageHeader *hdr, void *body, Address *sender, void *context)        
        {
            ClientRequest inReq; // Client request we get
            DOMRequest outReq;   // Outgoing request that we attach a deadline to

            VLOG(2) << "Received message from " << sender->GetIPAsString() << " " << hdr->msgLen;
            if (hdr->msgType == MessageType::CLIENT_REQUEST)
            {
                SignedMessageHeader *shdr = (SignedMessageHeader *) body;
                u_char *reqBytes = (u_char *)(shdr + 1);

                // TODO verify and handle signed header better
                if (!inReq.ParseFromArray(reqBytes, hdr->msgLen - shdr->sigLen - sizeof(SignedMessageHeader))) {
                    LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                    return;
                }
                

                outReq.set_send_time(GetMicrosecondTimestamp());
                outReq.set_deadline(outReq.send_time() + latencyBound_);
                outReq.set_proxy_id(proxyConfig_.proxyId);

                // TODO set these properly
                outReq.set_deadline_set_size(numReceivers_);
                outReq.set_late(false);

                outReq.set_client_req(body, hdr->msgLen);

                for (int i = 0; i < numReceivers_; i++)
                {
                    VLOG(2) << "Forwarding (" << inReq.client_id() << ", " << inReq.client_seq() << ") to " << receiverAddrs_[i].ip_;
                    forwardEps_[thread_id]->SignAndSendProtoMsgTo(receiverAddrs_[i], outReq, MessageType::DOM_REQUEST);
                }

                // Log* litem = new Log();
                // litem->clientId_ = request.clientid();
                // litem->reqId_ = request.reqid();
                // litem->clientTime_ = request.clienttime();
                // litem->proxyTime_ = request.sendtime();
                // litem->deadline_ = request.sendtime() + request.bound();
                // logs.assign(reqKey, litem);
                // litem->proxyEndProcessTime_ = GetMicrosecondTimestamp();
                // LOG(INFO) << "id=" << id << "\t"
                //           << "cid=" << request.clientid() << "\t" << request.reqid();

                // forwardCnt++;
                // if (forwardCnt == 1) {
                //   startTime = GetMicrosecondTimestamp();
                // } else if (forwardCnt % 100 == 0) {
                //   endTime = GetMicrosecondTimestamp();
                //   float rate = 100 / ((endTime - startTime) * 1e-6);
                //   LOG(INFO) << "Forward-Id=" << id << "\t"
                //             << "count =" << forwardCnt << "\t"
                //             << "rate=" << rate << " req/sec"
                //             << "\t"
                //             << "req is <" << request.clientid() << ","
                //             << request.reqid() << ">";
                //   startTime = endTime;
                // }
            }
        };

        /* Checks every 10ms to see if we are done*/
        auto checkEnd = [](void *ctx, void *receiverEP)
        {
            if (((Proxy *)ctx)->running_ == false)
            {
                ((Endpoint *)receiverEP)->LoopBreak();
            }
        };

        UDPMessageHandler handler(handleClientRequest);
        Timer monitor(checkEnd, 10, this);

        forwardEps_[thread_id]->RegisterMsgHandler(&handler);
        forwardEps_[thread_id]->RegisterTimer(&monitor);

        forwardEps_[thread_id]->LoopRun();
    }


} // namespace dombft
