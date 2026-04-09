#include "dummy_replica.h"

#include "lib/common.h"
#include "lib/config/config_util.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/tcp_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <sstream>

namespace dombft {
using namespace dombft::proto;

DummyReplica::DummyReplica(uint32_t replicaId, DummyProtocol prot, uint32_t batchSize)
    : replicaId_(replicaId)
    , prot_(prot)
    , batchSize_(batchSize)
    , nextSeq_(batchSize)
    , numVerifyThreads_(ConfigManager::getInstance().getConfig().replicaNumVerifyThreads)
    , sendThreadpool_(ConfigManager::getInstance().getConfig().replicaNumSendThreads)
    , useHMAC_(ConfigManager::getInstance().getConfig().clientUseHMAC)
{
    auto &configManager = ConfigManager::getInstance();
    const auto &config = configManager.getConfig();

    LOG(INFO) << "batchSize=" << batchSize_;

    const auto &replicaIps = configManager.getReplicaIps();
    std::string replicaIp = replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = configManager.getReplicaPort();
    LOG(INFO) << "replicaPort=" << replicaPort;

    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".der";
    LOG(INFO) << "Loading key from " << replicaKey;
    if (!sigProvider_.loadPrivateKey(replicaKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    LOG(INFO) << "Private key loaded";

    if (!sigProvider_.loadPublicKeys(NodeType::CLIENT, config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load replica public keys!";
        exit(1);
    }

    hmacProvider_.loadReplicaKeysDev(
        {NodeType::REPLICA, replicaId_}, configManager.getNumClients(), configManager.getNumReplicas()
    );

    // LOG(INFO) << "instantiating log";

    // if (config.app == AppType::COUNTER) {
    //     log_ = std::make_shared<Log>(std::make_shared<Counter>());
    // } else {
    //     LOG(ERROR) << "Unrecognized App Type";
    //     exit(1);
    // }
    // LOG(INFO) << "log instantiated";

    // Use ConfigManager's pre-calculated BFT parameters
    // f_ was already set above
    quorumSize_ = configManager.getQuorumSize();
    superQuorumSize_ = configManager.getSuperQuorumSize();

    if (config.transport == "nng") {
        auto addrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = configManager.getNumClients();
        size_t nProxies = configManager.getNumProxies();
        LOG(INFO) << "nClients=" << nClients;

        // First nClients addresses are for client connections
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(addrPairs[i].second);
        }

        // Then proxy addresses
        for (size_t i = nClients; i < nClients + nProxies; i++) {
            proxyAddrs_.push_back(addrPairs[i].second);
        }

        // Remaining addresses are for replica-to-replica connections
        for (size_t i = nClients + nProxies; i < addrPairs.size(); i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true, replicaAddrs_[replicaId_]);
    } else if (config.transport == "tcp") {
        auto addrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = configManager.getNumClients();
        size_t nProxies = configManager.getNumProxies();

        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(addrPairs[i].second);
        }
        for (size_t i = nClients; i < nClients + nProxies; i++) {
            proxyAddrs_.push_back(addrPairs[i].second);
        }
        for (size_t i = nClients + nProxies; i < addrPairs.size(); i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        endpoint_ = std::make_unique<TcpEndpointThreaded>(addrPairs, true, replicaAddrs_[replicaId_]);
    } else if (config.transport == "udp") {
        size_t nClients = configManager.getNumClients();
        const auto &clientIps = configManager.getClientIps();
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(Address(clientIps[i], configManager.getClientPort() + i));
        }

        const auto &replicaIps = configManager.getReplicaIps();
        for (size_t i = nClients + 1; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
        }

        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort);
    } else if (config.transport == "simple-rpc") {

        std::vector<Address> allAddrs;

        size_t nClients = configManager.getNumClients();
        const auto &clientIps = configManager.getClientIps();
        for (int i = 0; i < clientIps.size(); i++) {
            clientAddrs_.push_back(Address(clientIps[i], configManager.getClientPort() + i));
            allAddrs.push_back(clientAddrs_.back());
        }

        const auto &replicaIps = configManager.getReplicaIps();
        for (int i = 0; i < replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(replicaIps[i], configManager.getReplicaPort()));
            allAddrs.push_back(replicaAddrs_.back());
        }

        endpoint_ = std::make_unique<OOORPCEndpoint>(bindAddress, replicaPort, allAddrs, sendThreadpool_.size());

    } else {
        LOG(ERROR) << "Unsupported transport " << config.transport;
    }

    MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
    };

    endpoint_->RegisterMsgHandler(handler);

    endpoint_->RegisterSignalHandler([&]() {
        LOG(INFO) << "Received interrupt signal!";
        running_ = false;
        endpoint_->LoopBreak();
    });

    endpoint_->Connect();
}

DummyReplica::~DummyReplica()
{
    // TODO cleanup... though we don't really reuse this
}

void DummyReplica::run()
{
    // Submit first request
    LOG(INFO) << "Starting " << numVerifyThreads_ << " verify threads";
    running_ = true;
    for (int i = 0; i < numVerifyThreads_; i++) {
        verifyThreads_.emplace_back(&DummyReplica::verifyMessagesThd, this);
    }

    LOG(INFO) << "Starting process thread";
    processThread_ = std::thread(&DummyReplica::processMessagesThd, this);

    LOG(INFO) << "Starting main event loop...";
    endpoint_->LoopRun();
    LOG(INFO) << "Finishing main event loop...";

    for (std::thread &thd : verifyThreads_) {
        thd.join();
    }
    processThread_.join();
}

void DummyReplica::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    byte *rawMsg = (byte *) msgHdr;
    dombft::proto::DOMRequest request;

    if (msgHdr->msgType == DOM_REQUEST) {
        // Remove the DOM_HEADER for these messages

        if (!request.ParseFromArray(msgBuffer, msgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DOM_REQUEST message";
            return;
        }

        rawMsg = (byte *) request.client_req().c_str();
        msgHdr = (MessageHeader *) rawMsg;
    }

    std::vector<byte> msg(rawMsg, rawMsg + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen);

    // We skip verification of our own messages
    if (*sender == replicaAddrs_[replicaId_]) {
        processQueue_.enqueue(msg);
    } else {
        verifyQueue_.enqueue(msg);
    }

    VLOG(6) << verifyQueue_.size_approx() << " messages in verify queue, " << processQueue_.size_approx()
            << " messages in process queue";
}

void DummyReplica::verifyMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        if (!verifyQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == CLIENT_REQUEST || hdr->msgType == PS_CLIENT || hdr->msgType == PS_LEADER_FORWARD) {
            ClientRequest request;

            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST/PS_CLIENT/PS_LEADER_FORWARD message";
                continue;
            }

            bool verified = false;
            if (useHMAC_) {
                verified = hmacProvider_.verify(hdr, {NodeType::CLIENT, request.client_id()});

            } else {
                verified = sigProvider_.verify(hdr, {NodeType::CLIENT, request.client_id()});
            }

            if (!verified) {
                LOG(INFO) << "Failed to verify client signature from " << request.client_id();
                continue;
            }

            processQueue_.enqueue(msg);
        } else if (hdr->msgType == DUMMY_PROTO) {
            DummyProtocolMessage dummyProtoMsg;

            if (!dummyProtoMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse DUMMY_PROTO message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, dummyProtoMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature from " << dummyProtoMsg.replica_id();
                continue;
            }

            if (dummyProtoMsg.phase() == 0) {
                // Verify contained client requests

                bool clientSigs = true;

                for (uint32_t i = 0; i < dummyProtoMsg.client_reqs_size(); i++) {
                    const auto &req = dummyProtoMsg.client_reqs(i).req();
                    const std::string &sig = dummyProtoMsg.client_reqs(i).sig();

                    if (!sigProvider_.verify(req.SerializeAsString(), sig, {NodeType::CLIENT, req.client_id()})) {
                        LOG(INFO) << "Failed to verify client signature from " << req.client_id();
                        clientSigs = false;
                        break;
                    }
                }

                if (!clientSigs) {
                    continue;
                }
            }

            processQueue_.enqueue(msg);
        } else {
            LOG(ERROR) << "Verify thread does not handle message with unknown type " << (int) hdr->msgType;
            continue;
        }
    }
}

void DummyReplica::processMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        if (!processQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }
        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == MessageType::DOM_REQUEST) {
            DOMRequest domHeader;
            ClientRequest clientHeader;

            if (!domHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse DOM_REQUEST message";
                return;
            }

            // Separate this out into another function probably.
            MessageHeader *clientMsgHdr = (MessageHeader *) domHeader.client_req().c_str();
            byte *clientBody = (byte *) (clientMsgHdr + 1);
            if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

            processClientRequest(clientHeader, std::span{clientBody + clientMsgHdr->msgLen, clientMsgHdr->sigLen});
        }
        if (hdr->msgType == CLIENT_REQUEST || hdr->msgType == PS_CLIENT || hdr->msgType == PS_LEADER_FORWARD) {
            ClientRequest clientRequestMsg;

            if (!clientRequestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST/PS_CLIENT/PS_LEADER_FORWARD message";
                continue;
            }

            processClientRequest(clientRequestMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        } else if (hdr->msgType == DUMMY_PROTO) {
            DummyProtocolMessage protoMsg;

            if (!protoMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse DUMMY_PROTO message";
                continue;
            }

            // Preprepare
            if (protoMsg.phase() == 0) {
                if (prot_ == DummyProtocol::PBFT) {

                    protoMsg.set_phase(1);
                    protoMsg.set_replica_id(replicaId_);

                    for (uint32_t i = 0; i < protoMsg.client_reqs().size(); i++) {
                        protoMsg.mutable_client_reqs(i)->clear_req();
                    }

                    broadcastToReplicas(protoMsg, MessageType::DUMMY_PROTO);

                } else if (prot_ == DummyProtocol::ZYZ) {

                    for (uint32_t i = 0; i < protoMsg.client_reqs().size(); i++) {
                        auto &req = protoMsg.client_reqs(i);

                        Reply reply;
                        reply.set_replica_id(replicaId_);
                        reply.set_client_id(req.client_id());
                        reply.set_client_seq(req.client_seq());
                        reply.set_round(0);
                        reply.set_seq(protoMsg.seq() + i);
                        reply.set_replica_id(replicaId_);
                        reply.set_digest(std::string(32, '\0'));

                        sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[req.client_id()]);
                    }
                }

            }

            else if (protoMsg.phase() == 1) {
                if (prot_ == DummyProtocol::PBFT) {
                    uint32_t seq = protoMsg.seq();
                    if (seq <= committedSeq_)
                        continue;

                    prepareCounts[seq]++;

                    VLOG(5) << "PREPARE " << seq << " " << prepareCounts[seq] << " " << protoMsg.replica_id();

                    if (prepareCounts[seq] == quorumSize_) {
                        protoMsg.set_phase(2);
                        protoMsg.set_replica_id(replicaId_);

                        VLOG(2) << "PERF event=repair_prepared replica_id=" << replicaId_ << " seq=" << protoMsg.seq();

                        broadcastToReplicas(protoMsg, MessageType::DUMMY_PROTO);
                    }
                }
            }

            else if (protoMsg.phase() == 2) {
                if (prot_ == DummyProtocol::PBFT) {
                    uint32_t seq = protoMsg.seq();
                    if (seq <= committedSeq_)
                        continue;

                    commitCounts[seq]++;

                    VLOG(5) << "COMMIT " << seq << " " << commitCounts[seq] << " " << protoMsg.replica_id();

                    if (commitCounts[seq] == quorumSize_) {

                        VLOG(2) << "PERF event=committed replica_id=" << replicaId_ << " seq=" << protoMsg.seq();

                        // Use Repair Summary here since client only needs to see f + 1 of these
                        std::map<uint32_t, std::set<uint32_t>> clientSeqs;

                        for (uint32_t i = 0; i < protoMsg.client_reqs().size(); i++) {
                            auto &req = protoMsg.client_reqs(i);
                            clientSeqs[req.client_id()].insert(req.client_seq());
                        }

                        for (auto &[clientId, seqs] : clientSeqs) {
                            RepairSummary summary;

                            summary.set_round(0);
                            summary.set_replica_id(replicaId_);

                            for (uint32_t seq : seqs) {
                                CommittedReply reply;

                                reply.set_replica_id(replicaId_);
                                reply.set_client_id(clientId);
                                reply.set_client_seq(seq);
                                reply.set_seq(protoMsg.seq());

                                *(summary.add_replies()) = reply;
                            }

                            sendMsgToDst(summary, MessageType::REPAIR_SUMMARY, clientAddrs_[clientId]);
                        }

                        while (commitCounts[committedSeq_ + batchSize_] >= quorumSize_) {
                            committedSeq_ += batchSize_;

                            VLOG(2) << "PERF event=cleanup replica_id=" << replicaId_ << " seq=" << committedSeq_
                                    << " prepareCountsSize=" << prepareCounts.size()
                                    << " commitCountsSize=" << commitCounts.size();
                            commitCounts.erase(commitCounts.begin(), commitCounts.upper_bound(committedSeq_));
                            prepareCounts.erase(prepareCounts.begin(), prepareCounts.upper_bound(committedSeq_));
                        }
                    }
                }
            }
        }
    }
}

void DummyReplica::processClientRequest(const dombft::proto::ClientRequest &request, std::span<byte> sig)
{

    if (prot_ == DUMMY_DOM_BFT) {
        Reply reply;

        reply.set_replica_id(replicaId_);
        reply.set_client_id(request.client_id());
        reply.set_client_seq(request.client_seq());
        reply.set_round(0);
        reply.set_seq(0);   // Set seq to 0 here so clients see consistent messages ...
        reply.set_replica_id(replicaId_);
        reply.set_digest(std::string(32, '\0'));

        if (VLOG_IS_ON(2)) {
            VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << nextSeq_
                    << " client_id=" << request.client_id() << " client_seq=" << request.client_seq();
        } else if (VLOG_IS_ON(1)) {
            if (nextSeq_ % 1000 == 0) {
                VLOG(1) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << nextSeq_
                        << " client_id=" << request.client_id() << " client_seq=" << request.client_seq();
            }
        }

        //... but here increment nextSeq_ to keep track of requests received
        nextSeq_++;

        sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[request.client_id()]);

        return;
    }

    // For Zyz/PBFT, only leader handles client requests
    if (replicaId_ == 0) {

        batchQueue_.push_back({request, std::string(sig.begin(), sig.end())});

        if (batchQueue_.size() < batchSize_) {
            return;
        }

        DummyProtocolMessage preprepare;

        preprepare.set_phase(0);
        preprepare.set_replica_id(replicaId_);
        preprepare.set_seq(nextSeq_);

        for (const auto &[req, sig] : batchQueue_) {
            auto dummyReq = preprepare.add_client_reqs();

            dummyReq->set_client_id(req.client_id());
            dummyReq->set_client_seq(req.client_seq());
            *(dummyReq->mutable_req()) = req;
            dummyReq->set_sig(sig);
        }
        batchQueue_.clear();

        broadcastToReplicas(preprepare, MessageType::DUMMY_PROTO);

        VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << nextSeq_
                << " client_id=" << request.client_id() << " client_seq=" << request.client_seq();

        //... but here increment nextSeq_ to keep track of requests received
        nextSeq_ += batchSize_;
    }
}

// sending helpers
template <typename T> void DummyReplica::sendMsgToDst(const T &msg, MessageType type, const Address &dst)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        if (useHMAC_ && type == REPLY) {
            auto it = find(clientAddrs_.begin(), clientAddrs_.end(), dst);
            assert(it != clientAddrs_.end());

            uint32_t clientId = it - clientAddrs_.begin();

            hmacProvider_.appendMAC(hdr, SEND_BUFFER_SIZE, {NodeType::CLIENT, clientId});
        } else {
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        }
        endpoint_->SendPreparedMsgTo(dst, hdr);
    });
}

template <typename T> void DummyReplica::broadcastToReplicas(const T &msg, MessageType type)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr, hdr);
        }
    });
}

}   // namespace dombft