#include "proxy/proxy.h"

#include <openssl/pem.h>

#include "lib/common_type.h"

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
                                                        key));
        }
        measurmentEp_ = new UDPEndpoint(
            proxyConfig_.proxyIp, proxyConfig_.proxyMeasurmentPort);

        numReceivers_ = proxyConfig_.receiverIps.size();

        for (int i = 0; i < numReceivers_; i++)
        {
            std::string receiverIp = proxyConfig_.receiverIps[i];
            // TODO handle sharding for receivers
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
        for (auto &kv : threadPool_)
        {
            LOG(INFO) << "Join " << kv.first;
            kv.second->join();
            LOG(INFO) << "Join Complete " << kv.first;
        }
        LOG(INFO) << "Run Terminated ";
    }

    Proxy::~Proxy()
    {
        for (auto &kv : threadPool_)
        {
            delete kv.second;
        }

        // TODO Cleanup more

    }

    void Proxy::LaunchThreads()
    {
        int shardNum = proxyConfig_.proxyNumShards;

        threadPool_["RecvMeasurementsTd"] = new std::thread(&Proxy::RecvMeasurementsTd, this);

        for (int i = 0; i < shardNum; i++)
        {
            std::string key = "ForwardRequestsTd-" + std::to_string(i);
            threadPool_[key] = new std::thread(&Proxy::ForwardRequestsTd, this, i);
        }

        // std::string key = "LogTd";
        // threadPool_[key] = new std::thread(&Proxy::LogTd, this);
    }


    // TODO fix this
    void Proxy::LogTd()
    {
        Log litem;
        std::ofstream ofs("Proxy-Stats-" + std::to_string(proxyConfig_.proxyId) +
                          ".csv");
        ofs << "ReplicaId,ClientId,RequestId,ClientTime,ProxyTime,"
               "ProxyEndProcessTime,RecvTime,Deadline,"
               "FastReplyTime,"
               "SlowReplyTime,"
               "ProxyRecvTime,CommitType"
            << std::endl;
        uint32_t logCnt = 0;
        while (running_)
        {
            if (logQu_.try_dequeue(litem))
            {
                ofs << litem.ToString() << std::endl;
                logCnt++;
                if (logCnt % 10000 == 0)
                {
                    ofs.flush();
                }
            }
        }
    }

    void Proxy::RecvMeasurementsTd()
    {
        std::vector<uint32_t> replicaOWDs;

        MessageHandlerFunc handleMeasurementReply = [this, &replicaOWDs](MessageHeader *hdr, void *body, Address *sender, void *context)
        {
            MeasurementReply reply;

            if (reply.ParseFromArray(body, hdr->msgLen))
            {
                if (reply.owd() > 0)
                {
                    VLOG(1) << "replica=" << reply.receiver_id() << "\towd=" << reply.owd(); 

                    // TODO actually calculate something here?
                    replicaOWDs[reply.receiver_id()] = reply.owd();
                    // Update latency bound
                    uint32_t estimatedOWD = 0;
                    for (uint32_t i = 0; i < replicaOWDs.size(); i++)
                    {
                        if (estimatedOWD < replicaOWDs[i])
                        {
                            estimatedOWD = replicaOWDs[i];
                        }
                    }
                    if (estimatedOWD > maxOWD_)
                    {
                        estimatedOWD = maxOWD_;
                    }
                    latencyBound_.store(estimatedOWD);
                    VLOG(1) << "Update bound " << latencyBound_;
                }
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

        MessageHandler handler(handleMeasurementReply);
        Timer monitor(checkEnd, 10, this);

        measurmentEp_->RegisterMsgHandler(&handler);
        measurmentEp_->RegisterTimer(&monitor);

        measurmentEp_->LoopRun();
    }

    void Proxy::ForwardRequestsTd(const int thread_id)
    {
        // TODO add this logging back
        // ConcurrentMap<uint64_t, Log *> &logs = logMap_[id];
        // uint32_t forwardCnt = 0;
        // uint64_t startTime, endTime;

        MessageHandlerFunc handleClientRequest = [this, thread_id](MessageHeader *hdr, void *body, Address *sender, void *context)
        {
            ClientRequest inReq; // Client request we get
            DOMRequest outReq;   // Outgoing request that we attach a deadline to

            if (hdr->msgType == MessageType::CLIENT_REQUEST)
            {
                // TODO verify and handle signed header better
                if (!inReq.ParseFromArray(body + sizeof(SignedMessageHeader), hdr->msgLen)) {
                    LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                    return;
                }
                
                outReq.set_deadline(GetMicrosecondTimestamp() + latencyBound_);
                outReq.set_proxy_id(proxyConfig_.proxyId);

                // TODO set these properly
                outReq.set_deadline_set_size(numReceivers_);
                outReq.set_late(false);

                outReq.set_client_req(body, hdr->msgLen);

                for (int i = 0; i < numReceivers_; i++)
                {
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

        MessageHandler handler(handleClientRequest);
        Timer monitor(checkEnd, 10, this);

        forwardEps_[thread_id]->RegisterMsgHandler(&handler);
        forwardEps_[thread_id]->RegisterTimer(&monitor);

        forwardEps_[thread_id]->LoopRun();
    }


} // namespace dombft
