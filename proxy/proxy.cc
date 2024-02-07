#include "proxy/proxy.h"

#include "signed_udp_endpoint.h"

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
        CreateContext();
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


        // for (uint32_t i = 0; i < committedReplyMap_.size(); i++) {
        //   ConcurrentMap<uint64_t, Reply*>& committedReply = committedReplyMap_[i];
        //   ConcurrentMap<uint64_t, Reply*>::Iterator iter(committedReply);
        //   while (iter.isValid()) {
        //     Reply* reply = iter.getValue();
        //     if (reply) {
        //       delete reply;
        //     }
        //     iter.next();
        //   }
        // }
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
        int recvlen = 0;
        char buffer[UDP_BUFFER_SIZE];
        MessageHeader *msgHdr = (MessageHeader *)(void *)buffer;

        struct sockaddr_in recvAddr;
        socklen_t sockLen = sizeof(recvAddr);

        MeasurementReply reply;
        uint32_t replyNum = 0;
        uint64_t startTime, endTime;

        while (running_)
        {
            if ((recvlen = recvfrom(forwardFds_[, buffer, UDP_BUFFER_SIZE, 0,
                                    (struct sockaddr *)(&recvAddr), &sockLen)) > 0)
            {
                if ((uint32_t)recvlen < sizeof(MessageHeader) ||
                    (uint32_t)recvlen < msgHdr->msgLen + sizeof(MessageHeader))
                {
                    continue;
                }

                if (reply.ParseFromArray(buffer + sizeof(MessageHeader),
                                         msgHdr->msgLen))
                {
                    if (reply.owd() > 0)
                    {
                        VLOG(1) << "replica=" << reply.replica_id << "\towd=" << owdSample.second;
                        replicaOWDs[owdSample.first] = owdSample.second;
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
            }
        }
    }

    void Proxy::ForwardRequestsTd(const int id)
    {
        ConcurrentMap<uint64_t, Log *> &logs = logMap_[id];
        char buffer[UDP_BUFFER_SIZE];
        MessageHeader *msgHdr = (MessageHeader *)(void *)buffer;
        int sz = -1;
        struct sockaddr_in receiverAddr;
        socklen_t len = sizeof(receiverAddr);
        Request request;
        uint32_t forwardCnt = 0;
        uint64_t startTime, endTime;

        while (running_)
        {
            if ((sz = recvfrom(requestReceiveFds_[id], buffer, UDP_BUFFER_SIZE, 0,
                               (struct sockaddr *)&receiverAddr, &len)) > 0)
            {
                if ((uint32_t)sz < sizeof(MessageHeader) ||
                    (uint32_t)sz < msgHdr->msgLen + sizeof(MessageHeader))
                {
                    continue;
                }
                if (msgHdr->msgType == MessageType::CLIENT_REQUEST &&
                    request.ParseFromArray(buffer + sizeof(MessageHeader),
                                           msgHdr->msgLen))
                {
                    uint64_t reqKey = CONCAT_UINT32(request.clientid(), request.reqid());
                    request.set_bound(latencyBound_);
                    request.set_proxyid(proxyIds_[id]);
                    request.set_sendtime(GetMicrosecondTimestamp());

                    std::string msg = request.SerializeAsString();
                    msgHdr->msgType = MessageType::CLIENT_REQUEST;
                    msgHdr->msgLen = msg.length();
                    memcpy(buffer + sizeof(MessageHeader), msg.c_str(), msg.length());
                    if (clientAddrs_.get(request.clientid()) == NULL)
                    {
                        struct sockaddr_in *addr = new sockaddr_in(receiverAddr);
                        clientAddrs_.assign(request.clientid(), addr);
                    }

                    // Send to every replica
                    for (int i = 0; i < replicaNum_; i++)
                    {
                        // uint32_t generateProxyId = (uint32_t)(proxyIds_[id] >> 32u);
                        // struct sockaddr_in* replicaAddr =
                        //     replicaAddrs_[i][generateProxyId % replicaAddrs_[i].size()];
                        struct sockaddr_in *replicaAddr =
                            replicaAddrs_[i][proxyIds_[id] % replicaAddrs_[i].size()];

                        sendto(forwardFds_[id], buffer,
                               msgHdr->msgLen + sizeof(MessageHeader), 0,
                               (struct sockaddr *)replicaAddr, sizeof(sockaddr_in));
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
            }
        }
    }

    void Proxy::CreateContext()
    {
        running_ = true;
        int numShards = proxyConfig_.proxyNumShards;
        uint32_t proxyId = proxyConfig_.proxyId;
        latencyBound_ = proxyConfig_.replicaInitialOwd;
        maxOWD_ = proxyConfig_.proxyMaxOwd;

        logMap_.resize(numShards);

        for (int i = 0; i < numShards; i++) {
            forwardFds_[i] = SignedUDPEndpoint(proxyConfig_.proxyIp,
                                            proxyConfig_.proxyReplyPortBase + i);
            requestReceiveFds_[i] = CreateSocketFd(
                proxyConfig_.proxyIp, proxyConfig_.proxyRequestPortBase + i);
            replyFds_[i] = CreateSocketFd("", -1);
            proxyIds_[i] = ((proxyIds_[i] << 32) | (uint32_t)i);
        }



        numReplicas_ = proxyConfig_.replicaIps.size();
        f_ = numReplicas_ / 3;

        for (int i = 0; i < numReplicas_; i++)
        {
            std::string replicaIP = proxyConfig_.replicaIps[i];
            for (int j = 0; j < proxyConfig_.replicaReceiverShards; j++)
            {
                struct sockaddr_in *addr = new sockaddr_in();
                bzero(addr, sizeof(struct sockaddr_in));
                addr->sin_family = AF_INET;
                addr->sin_port = htons(proxyConfig_.replicaReceiverPort + j);
                addr->sin_addr.s_addr = inet_addr(replicaIP.c_str());
                replicaAddrs_[i].push_back(addr);
            }
        }
    }

} // namespace dombft
