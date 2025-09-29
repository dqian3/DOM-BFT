#include "replica.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/apps/kv_store.h"
#include "lib/common.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <algorithm>
#include <cryptopp/sha.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(
    const ProcessConfig &config, uint32_t replicaId, bool crashed, uint32_t swapFreq, uint32_t viewChangeFreq,
    bool commitLocalInViewChange, uint32_t viewChangeNum, uint32_t checkpointDropFreq, bool skipForwarding,
    bool ignoreDeadlines
)
    : replicaId_(replicaId)
    , f_(config.resiliency == "5f+1" ? config.replicaIps.size() / 5 : config.replicaIps.size() / 3)
    , quorumSize_(config.resiliency == "5f+1" ? 4 * f_ + 1 : 2 * f_ + 1)
    , superQuorumSize_(config.resiliency == "5f+1" ? 4 * f_ + 1 : 3 * f_ + 1)
    , checkpointInterval_(config.replicaCheckpointInterval)
    , snapshotInterval_(config.replicaSnapshotInterval)
    , numVerifyThreads_(config.replicaNumVerifyThreads)
    , useHMAC_(config.clientUseHMAC)
    , repairTimeout_(config.replicaRepairTimeout)
    , repairViewTimeout_(config.replicaRepairViewTimeout)
    , proxyPort_(config.proxyForwardPort)
    , numReceivers_(config.replicaIps.size())
    , skipForwarding_(skipForwarding)
    , ignoreDeadlines_(ignoreDeadlines)
    , sigProvider_()
    , sendThreadpool_(config.replicaNumSendThreads)
    , running_(true)
    , round_(1)
    , checkpointCollectors_(replicaId_, quorumSize_)
    , crashed_(crashed)
    , swapFreq_(swapFreq)
    , checkpointDropFreq_(checkpointDropFreq)
    , viewChangeFreq_(viewChangeFreq)
    , viewChangeInst_(viewChangeFreq_)
    , commitLocalInViewChange_(commitLocalInViewChange)
    , viewChangeNum_(viewChangeNum)
{
    // Replica initialization
    std::string replicaIp = config.replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = config.replicaPort;
    LOG(INFO) << "replicaPort=" << replicaPort;

    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".der";
    LOG(INFO) << "Loading replica key from " << replicaKey;
    if (!sigProvider_.loadPrivateKey(replicaKey)) {
        LOG(ERROR) << "Unable to load replica private key!";
        exit(1);
    }

    // For unified process, we use the replica key for both replica and receiver functions

    LOG(INFO) << "Private keys loaded";

    if (!sigProvider_.loadPublicKeys(NodeType::CLIENT, config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys(NodeType::REPLICA, config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load replica public keys!";
        exit(1);
    }

    hmacProvider_.loadReplicaKeysDev({NodeType::REPLICA, replicaId_}, config.clientIps.size());

    LOG(INFO) << "Instantiating log and application";

    if (config.app == AppType::COUNTER) {
        app_ = std::make_shared<Counter>();
    } else if (config.app == AppType::KV_STORE) {
        app_ = std::make_shared<KVStore>();
    } else {
        LOG(ERROR) << "Unrecognized App Type";
        exit(1);
    }
    log_ = std::make_shared<Log>(app_);
    LOG(INFO) << "Log instantiated";

    // Network setup for unified functionality
    if (config.transport == "nng") {
        // Use replica addressing for unified process
        auto replicaAddrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = config.clientIps.size();
        for (size_t i = 0; i < nClients; i++) {
            clientAddrs_.push_back(replicaAddrPairs[i].second);
        }

        receiverAddr_ = replicaAddrPairs[nClients].second;

        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(replicaAddrPairs[nClients + 1 + i].second);
        }

        endpoint_ = std::make_unique<NngEndpointThreaded>(replicaAddrPairs, true);

    } else if (config.transport == "simple-rpc") {
        std::vector<Address> addrs;
        replicaAddr_ = Address(config.replicaIps[replicaId], config.replicaPort);
        addrs.push_back(replicaAddr_);

        // Add proxy addresses for receiver functionality
        for (uint32_t i = 0; i < config.proxyIps.size(); i++) {
            addrs.push_back(Address(config.proxyIps[i], config.proxyForwardPort));
        }

        // Add replica addresses
        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
            if (i != replicaId_) {
                addrs.push_back(Address(config.replicaIps[i], config.replicaPort));
            }
        }

        // Add client addresses
        for (uint32_t i = 0; i < config.clientIps.size(); i++) {
            clientAddrs_.push_back(Address(config.clientIps[i], config.clientPort));
        }

        endpoint_ = std::make_unique<OOORPCEndpoint>(bindAddress, replicaPort, addrs);

    } else {
        // UDP setup
        replicaAddr_ = Address(config.replicaIps[replicaId], config.replicaPort);

        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
        }

        for (uint32_t i = 0; i < config.clientIps.size(); i++) {
            clientAddrs_.push_back(Address(config.clientIps[i], config.clientPort));
        }

        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);
    }

    // Set up timer for receiver deadline checking
    fwdTimer_ =
        std::make_unique<Timer>([](void *ctx, void *endpoint) { ((Replica *) ctx)->checkDeadlines(); }, 1000, this);
    ev_set_priority(fwdTimer_->evTimer_, EV_MAXPRI);
    endpoint_->RegisterTimer(fwdTimer_.get());

    // Register unified message handler
    endpoint_->RegisterMsgHandler([this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
        this->checkDeadlines();   // Check deadlines after each message
    });

    endpoint_->RegisterSignalHandler([&]() {
        running_ = false;
        endpoint_->LoopBreak();
    });

    LOG(INFO) << "Starting verify threads for replica functionality";
    for (int i = 0; i < numVerifyThreads_; i++) {
        verifyThreads_.emplace_back(&Replica::verifyMessagesThd, this);
    }

    LOG(INFO) << "Starting verify threads for receiver functionality";
    uint32_t numReceiverVerifyThreads = config.replicaNumVerifyThreads;
    for (int i = 0; i < numReceiverVerifyThreads; i++) {
        receiverVerifyThreads_.emplace_back(&Replica::receiverVerifyThd, this, i);
    }

    processThread_ = std::thread(&Replica::processMessagesThd, this);

    LOG(INFO) << "Replica initialized successfully";
}

Replica::~Replica()
{
    running_ = false;

    for (std::thread &thd : verifyThreads_) {
        if (thd.joinable()) {
            thd.join();
        }
    }

    for (std::thread &thd : receiverVerifyThreads_) {
        if (thd.joinable()) {
            thd.join();
        }
    }

    if (processThread_.joinable()) {
        processThread_.join();
    }
}

void Replica::run()
{
    endpoint_->Connect();
    LOG(INFO) << "Starting unified replica main loop";
    endpoint_->LoopRun();

    LOG(INFO) << "Replica exited cleanly";
}

void Replica::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    if (msgHdr->msgLen < 0) {
        return;
    }

    LOG(INFO) << "Received message of type " << (int) msgHdr->msgType << " from " << *sender;

    // Handle receiver-specific messages (from proxies)
    if (msgHdr->msgType == MessageType::DOM_REQUEST) {
        receiveRequest(msgHdr, msgBuffer, sender);
        return;
    }

    // Handle replica-specific messages
    // Skip verification of our own messages and receiver messages
    if (sender->ip() == replicaAddrs_[replicaId_].ip() || sender->ip() == receiverAddr_.ip()) {
        processQueue_.enqueue(std::vector<byte>(msgBuffer, msgBuffer + sizeof(MessageHeader) + msgHdr->msgLen));
        return;
    }

    // Queue for verification
    verifyQueue_.enqueue(std::vector<byte>(msgBuffer, msgBuffer + sizeof(MessageHeader) + msgHdr->msgLen));
}

// Receiver functionality implementation
void Replica::receiveRequest(MessageHeader *hdr, byte *body, Address *sender)
{
#if FABRIC_CRYPTO
    if (!sigProvider_.verify(hdr, "proxy", 0)) {
        LOG(INFO) << "Failed to verify proxy signature";
        return;
    }
#endif

    DOMRequest request;
    if (!request.ParseFromArray(body, hdr->msgLen)) {
        LOG(ERROR) << "Unable to parse DOM_REQUEST message";
        return;
    }

    int64_t recv_time = GetMicrosecondTimestamp();
    VLOG(3) << "RECEIVE c_id=" << request.client_id() << " c_seq=" << request.client_seq() << " Measured delay "
            << recv_time - request.send_time() << " usec";

    if (recv_time > request.deadline()) {
        request.set_late(true);
        VLOG(1) << "Request is late by " << recv_time - request.deadline() << "us";
    }

    uint64_t deadline = request.deadline();
    if (ignoreDeadlines_) {
        deadline = recv_time;
    }

    auto r = std::make_shared<ReceiverRequest>();
    r->request = request;
    r->deadline = request.deadline();
    r->clientId = request.client_id();
    r->verified = false;

    {
        std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
        deadlineQueue_[{deadline, request.client_id()}] = r;
    }

    receiverVerifyQueue_.enqueue(r);

    // Send measurement replies back to the proxy
    if (recv_time - lastMeasurementTimes_[request.proxy_id()] > 5000) {
        lastMeasurementTimes_[request.proxy_id()] = recv_time;

        MeasurementReply mReply;
        mReply.set_receiver_id(replicaId_);
        mReply.set_owd(recv_time - request.send_time());
        mReply.set_send_time(request.send_time());
        MessageHeader *replyHdr = endpoint_->PrepareProtoMsg(mReply, MessageType::MEASUREMENT_REPLY);
#if FABRIC_CRYPTO
        sigProvider_.appendSignature(replyHdr, SEND_BUFFER_SIZE);
#endif
        endpoint_->SendPreparedMsgTo(Address(sender->ip(), proxyPort_), replyHdr);
    }
}

void Replica::forwardRequest(const DOMRequest &request)
{
    uint64_t now = GetMicrosecondTimestamp();

    VLOG(2) << "Forwarding request " << now - request.deadline() << "us after deadline "
            << "c_id=" << request.client_id() << " c_seq=" << request.client_seq();

    numForwarded_++;
    lastFwdDeadline_ = request.deadline();

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(request, MessageType::DOM_REQUEST);
#if FABRIC_CRYPTO
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
    if (!skipForwarding_) {
        // Forward to replica (self)
        endpoint_->SendPreparedMsgTo(replicaAddr_, hdr);
    }
}

void Replica::checkDeadlines()
{
    std::lock_guard<std::mutex> guard(deadlineQueueMtx_);

    uint64_t now = GetMicrosecondTimestamp();
    auto it = deadlineQueue_.begin();

    while (it != deadlineQueue_.end() && it->first.first <= now) {
        VLOG(3) << "Deadline " << it->first.first << " reached now=" << now;

        if (!it->second->verified) {
            VLOG(3) << "Request not verified, waiting for next check";
            break;
        }

        forwardRequest(it->second->request);
        auto temp = std::next(it);
        deadlineQueue_.erase(it);
        it = temp;
    }

    int64_t nextCheck = deadlineQueue_.empty() ? 1000 : (int64_t) deadlineQueue_.begin()->first.first - now;
    nextCheck = std::max(1000l, nextCheck);

    endpoint_->ResetTimer(fwdTimer_.get(), nextCheck);
}

void Replica::receiverVerifyThd(int threadId)
{
    LOG(INFO) << "Starting receiver verify thread " << threadId;

    uint32_t numVerified = 0;
    std::shared_ptr<ReceiverRequest> request;
    while (running_) {
        if (!receiverVerifyQueue_.wait_dequeue_timed(request, 10000)) {
            continue;
        }

        ClientRequest clientHeader;
        MessageHeader *clientMsgHdr = (MessageHeader *) request->request.client_req().c_str();
        byte *clientBody = (byte *) (clientMsgHdr + 1);

        if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            continue;
        }

        bool verified = false;
        if (useHMAC_) {
            verified = hmacProvider_.verify(clientMsgHdr, {NodeType::CLIENT, request->clientId});
        } else {
            verified = sigProvider_.verify(clientMsgHdr, {NodeType::CLIENT, request->clientId});
        }

        {
            std::lock_guard<std::mutex> guard(deadlineQueueMtx_);
            if (verified) {
                VLOG(4) << "Verified client signature for c_id=" << request->clientId
                        << " c_seq=" << request->request.client_seq();
                request->verified = true;
            } else {
                VLOG(1) << "Failed to verify client signature!";
                deadlineQueue_.erase({request->deadline, request->clientId});
            }
        }

        numVerified++;
    }

    LOG(INFO) << "Receiver verify thread " << threadId << " verified " << numVerified << " requests";
}

// Replica functionality methods from original replica.cc

void Replica::verifyMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        if (!verifyQueue_.wait_dequeue_timed(msg, 50000)) {
            continue;
        }

        MessageHeader *hdr = (MessageHeader *) msg.data();
        byte *body = (byte *) (hdr + 1);

        if (hdr->msgType == CERT) {
            Cert cert;

            if (!cert.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CERT message";
                continue;
            }

            if (!verifyCert(cert)) {
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPLY) {
            Reply reply;

            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == COMMIT) {
            Commit commitMsg;

            if (!commitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, commitMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == SNAPSHOT_REQUEST) {
            SnapshotRequest request;
            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REQUEST message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, request.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == SNAPSHOT_REPLY) {
            SnapshotReply reply;
            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REPLY message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, reply.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }
            if (!verifyCheckpoint(reply.checkpoint())) {
                LOG(INFO) << "Failed to verify checkpoint from replica " << reply.replica_id() << " in SNAPSHOT_REPLY!";
                continue;
            }

            if (reply.has_snapshot_checkpoint() && !verifyCheckpoint(reply.snapshot_checkpoint())) {
                LOG(INFO) << "Failed to verify snapshot checkpoint from replica " << reply.replica_id()
                          << " in SNAPSHOT_REPLY!";
                continue;
            }

            processQueue_.enqueue(msg);
        }
#if !USE_PROXY

        else if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest requestMsg;

            if (!requestMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, requestMsg.client_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

#endif

        // Repair related

        else if (hdr->msgType == REPAIR_CLIENT_TIMEOUT) {
            RepairClientTimeout timeoutMsg;

            if (!timeoutMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_CLIENT_TIMEOUT message";
                return;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, timeoutMsg.client_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_REPLICA_TIMEOUT) {
            RepairReplicaTimeout timeoutMsg;

            if (!timeoutMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLICA_TIMEOUT message";
                return;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, timeoutMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_REPLY_PROOF) {
            RepairReplyProof proofMsg;

            if (!proofMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLY_PROOF message";
                return;
            }

            if (!verifyRepairReplyProof(proofMsg)) {
                // TODO should be LOG(WARNING)
                VLOG(2) << "Failed to verify repair reply proof!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT_PROOF) {
            RepairTimeoutProof proofMsg;

            if (!proofMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TRIGGER message";
                return;
            }

            if (!verifyRepairTimeoutProof(proofMsg)) {
                LOG(WARNING) << "Failed to verify repair timeout proof!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_START) {
            RepairStart repairStartMsg;
            if (!repairStartMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_START message";
                continue;
            }

            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, repairStartMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == REPAIR_DONE) {
            RepairDone repairDoneMsg;
            if (!repairDoneMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_DONE message";
                continue;
            }

            if (!verifyRepairDone(repairDoneMsg)) {
                LOG(INFO) << "Failed to verify REPAIR_DONE message from " << repairDoneMsg.replica_id();
                continue;
            }

            processQueue_.enqueue(msg);
        }

        else if (hdr->msgType == PBFT_PREPREPARE) {
            PBFTPrePrepare PBFTPrePrepareMsg;
            if (!PBFTPrePrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrePrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTPrePrepareMsg.primary_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }

            // Verify digest and logs in proposal
            RepairProposal proposal = PBFTPrePrepareMsg.proposal();
            if (getProposalDigest(proposal) != PBFTPrePrepareMsg.proposal_digest()) {
                LOG(INFO) << "Proposal digest does not match!";
                continue;
            }

            if (!verifyRepairProposal(proposal)) {
                LOG(INFO) << "Failed to verify repair proposal!";
                assert(false);   // TODO remove later
                continue;
            }

            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_PREPARE) {
            PBFTPrepare PBFTPrepareMsg;
            if (!PBFTPrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTPrepareMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_COMMIT) {
            PBFTCommit PBFTCommitMsg;
            if (!PBFTCommitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTCommit message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, PBFTCommitMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_VIEWCHANGE) {
            PBFTViewChange viewChangeMsg;
            if (!viewChangeMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTViewChange message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, viewChangeMsg.replica_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            if (!verifyViewChange(viewChangeMsg))
                continue;
            processQueue_.enqueue(msg);

        } else if (hdr->msgType == PBFT_NEWVIEW) {
            PBFTNewView newViewMsg;
            if (!newViewMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTNewView message";
                continue;
            }
            if (!sigProvider_.verify(hdr, {NodeType::REPLICA, newViewMsg.primary_id()})) {
                LOG(INFO) << "Failed to verify primary replica signature for PBFTNewView!";
                continue;
            }
            const auto &viewChanges = newViewMsg.view_changes();
            const auto &sigs = newViewMsg.view_change_sigs();
            bool success = true;
            for (int i = 0; i < viewChanges.size(); i++) {
                if (!sigProvider_.verify(
                        viewChanges[i].SerializeAsString(), sigs[i], {NodeType::REPLICA, viewChanges[i].replica_id()}
                    )) {
                    LOG(INFO) << "Failed to verify replica signature in new view!";
                    success = false;
                    break;
                }
            }
            if (!success)
                continue;
            processQueue_.enqueue(msg);
        } else {
            // DOM_Requests from the receiver skip this step. We should drop
            // request types from other processes.
            LOG(ERROR) << "Verify thread does not handle message with unknown type " << (int) hdr->msgType;
        }
    }
}

void Replica::processMessagesThd()
{
    // TODO we do some redundant work deserializing messages here
    std::vector<byte> msg;

    while (running_) {
        // Check for timeouts each time before processing a message
        checkTimeouts();

        // Check to see if any snapshots are ready
        std::pair<uint32_t, AppSnapshot> snapshot;
        if (snapshotQueue_.try_dequeue(snapshot)) {
            processSnapshot(snapshot.second, snapshot.first);
        }

        if (!processQueue_.wait_dequeue_timed(msg, 100000)) {
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
                continue;
            }

            // TODO Hack to pass through deadline lol
            clientHeader.set_deadline(domHeader.deadline());

            if (repair_) {
                VLOG(6) << "Queuing request due to repair";
                repairQueuedReqs_.insert({{domHeader.deadline(), clientHeader.client_id()}, clientHeader});
                continue;
            }

            if (swapFreq_ && log_->getNextSeq() % swapFreq_ == 0)
                holdAndSwapCliReq(clientHeader);
            else
                processClientRequest(clientHeader);
        }

#if !USE_PROXY

        else if (hdr->msgType == CLIENT_REQUEST) {
            ClientRequest clientHeader;

            if (!clientHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            processClientRequest(clientHeader);
        }

#endif

        if (hdr->msgType == CERT) {
            Cert cert;

            if (!cert.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CERT message";
                return;
            }

            processCert(cert);
        } else if (hdr->msgType == REPLY) {
            Reply replyHeader;

            if (!replyHeader.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPLY message";
                return;
            }

            processReply(replyHeader, std::span{body + hdr->msgLen, hdr->sigLen});
        } else if (hdr->msgType == COMMIT) {
            Commit commitMsg;

            if (!commitMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                return;
            }

            processCommit(commitMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == SNAPSHOT_REQUEST) {
            SnapshotRequest request;
            if (!request.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REQUEST message";
                return;
            }
            processSnapshotRequest(request);
        } else if (hdr->msgType == SNAPSHOT_REPLY) {
            SnapshotReply reply;
            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse SNAPSHOT_REPLY message";
                return;
            }
            processSnapshotReply(reply);
        }

        else if (hdr->msgType == REPAIR_CLIENT_TIMEOUT) {
            RepairClientTimeout msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_CLIENT_TIMEOUT message";
                return;
            }

            processRepairClientTimeout(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == REPAIR_REPLICA_TIMEOUT) {
            RepairReplicaTimeout msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLICA_TIMEOUT message";
                return;
            }

            processRepairReplicaTimeout(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        else if (hdr->msgType == REPAIR_REPLY_PROOF) {
            RepairReplyProof msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_REPLY_PROOF message";
                return;
            }

            processRepairReplyProof(msg);
        }

        else if (hdr->msgType == REPAIR_TIMEOUT_PROOF) {
            RepairTimeoutProof msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_TRIGGER message";
                return;
            }

            processRepairTimeoutProof(msg);
        }

        if (hdr->msgType == REPAIR_START) {
            RepairStart msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_START message";
                return;
            }
            processRepairStart(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == REPAIR_DONE) {
            RepairDone msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse REPAIR_DONE message";
                return;
            }

            processRepairDone(msg);
        }

        if (hdr->msgType == PBFT_PREPREPARE) {
            PBFTPrePrepare msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_PREPREPARE message";
                return;
            }

            processPrePrepare(msg);
        }

        if (hdr->msgType == PBFT_PREPARE) {
            PBFTPrepare msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_PREPARE message";
                return;
            }

            processPrepare(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == PBFT_COMMIT) {
            PBFTCommit msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_COMMIT message";
                return;
            }

            processPBFTCommit(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == PBFT_VIEWCHANGE) {
            PBFTViewChange msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_VIEWCHANGE message";
                return;
            }
            processPBFTViewChange(msg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == PBFT_NEWVIEW) {
            PBFTNewView msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFT_NEWVIEW message";
                return;
            }

            processPBFTNewView(msg);
        }
    }
}

void Replica::checkTimeouts()
{
    uint64_t now = GetMicrosecondTimestamp();

    if (repairTimeoutStart_ != 0 && now - repairTimeoutStart_ > repairTimeout_) {
        repairTimeoutStart_ = 0;
        LOG(WARNING) << "repairStartTimer for round=" << round_ << " timed out! Sending timeout message!";

        RepairReplicaTimeout msg;
        msg.set_round(round_);
        msg.set_replica_id(replicaId_);

        broadcastToReplicas(msg, MessageType::REPAIR_REPLICA_TIMEOUT);
    };

    now = GetMicrosecondTimestamp();

    if (repairViewStart_ != 0 && now - repairViewStart_ > repairViewTimeout_) {
        repairViewStart_ = now;
        // TODO VC timer should be cancelled and restarted after receiving 2f + 1 VC messages

        LOG(WARNING) << "Repair for round=" << round_ << " pbft_view=" << pbftView_ << " failed (timed out)!";
        pbftViewChanges_.clear();
        pbftViewChangeSigs_.clear();
        this->startViewChange();
    };
}

// ========== Replica Message Processing ==========

void Replica::processClientRequest(const proto::ClientRequest &request, bool queued)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();

    if (clientId < 0 || clientId > clientAddrs_.size()) {
        LOG(ERROR) << "Invalid client id" << clientId;
        return;
    }

    // 1. Check if client request has been executed in latest checkpoint (i.e. is committed), in which case
    // we should return a CommittedReply, and client only needs f + 1
    if (log_->getCommittedCheckpoint().clientRecord_.contains(clientId, clientSeq)) {
        VLOG(4) << "DUP request c_id=" << clientId << " c_seq=" << clientSeq
                << " has been committed in previous checkpoint/repair!, Sending committed reply";

        CommittedReply reply;

        reply.set_replica_id(replicaId_);
        reply.set_client_id(clientId);
        reply.set_client_seq(clientSeq);
        reply.set_is_repair(false);

        // TODO Cache for client results not implemented, so blank results are sent that all look the same

        sendMsgToDst(reply, MessageType::COMMITTED_REPLY, clientAddrs_[clientId]);
        return;
    }

    std::string result;
    uint32_t seq;

    if (!log_->addEntry(clientId, clientSeq, request.req_data(), result)) {
        VLOG(2) << "DUP request c_id=" << clientId << " c_seq=" << clientSeq
                << " is added into log, but not committed, dropping!";
        return;
    }

    seq = log_->getNextSeq() - 1;
    log_->getEntry(seq).deadline = request.deadline();

    VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << seq << " client_id=" << clientId
            << " client_seq=" << clientSeq << " round=" << round_ << " digest=" << digest_to_hex(log_->getDigest())
            << " queued=" << queued;

    Reply reply;
    reply.set_client_id(clientId);
    reply.set_client_seq(clientSeq);
    reply.set_replica_id(replicaId_);
    reply.set_result(result);
    reply.set_seq(seq);
    reply.set_round(round_);
    reply.set_digest(log_->getDigest());
    reply.set_queued(queued);

    sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);

    // Try and commit every checkpointInterval replies
    if (seq % checkpointInterval_ == 0) {
        // Save a digest of the application state and also save a snapshot
        startCheckpoint(seq % snapshotInterval_ == 0);
    }
}

void Replica::holdAndSwapCliReq(const proto::ClientRequest &request)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();
    if (!heldRequest_) {
        heldRequest_ = request;
        VLOG(2) << "Holding request (" << clientId << ", " << clientSeq << ") for swapping";
        return;
    }
    processClientRequest(request);
    processClientRequest(heldRequest_.value());
    VLOG(2) << "Swapped requests (" << clientId << ", " << clientSeq << ") and (" << heldRequest_->client_id() << ", "
            << heldRequest_->client_seq() << ")";
    heldRequest_.reset();
}

void Replica::processCert(const Cert &cert)
{
    // TODO make sure this works
    const Reply &r = cert.replies()[0];
    CertReply reply;

    if (cert.round() < round_) {
        VLOG(2) << "Received stale cert with round " << cert.round() << " < " << round_ << " for seq=" << r.seq()
                << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    if (repair_) {
        VLOG(2) << "Cannot accept cert for seq=" << r.seq() << " from " << r.replica_id() << " due to ongoing repair";
        return;
    }

    if (!log_->addCert(cert.seq(), cert)) {
        VLOG(2) << "Failed to add cert for seq=" << r.seq() << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    reply.set_replica_id(replicaId_);
    reply.set_round(round_);
    reply.set_client_id(r.client_id());
    reply.set_client_seq(r.client_seq());
    reply.set_seq(r.seq());

    VLOG(3) << "Sending cert ack for seq=" << r.seq() << " c_id=" << reply.client_id()
            << " cseq=" << reply.client_seq();

    sendMsgToDst(reply, MessageType::CERT_REPLY, clientAddrs_[reply.client_id()]);
}

void Replica::processReply(const proto::Reply &reply, std::span<byte> sig)
{
    uint32_t rSeq = reply.seq();

    if (repair_) {
        VLOG(2) << "Ignoring reply for seq=" << rSeq << " from " << reply.replica_id() << " due to ongoing repair";
        return;
    }

    if (reply.round() < round_) {
        VLOG(4) << "Checkpoint reply seq=" << rSeq << " round outdated, skipping";
        return;
    }
    VLOG(3) << "Processing reply from replica " << reply.replica_id() << " for seq " << rSeq;

    auto &checkpoint = log_->getCommittedCheckpoint();

    if (rSeq <= checkpoint.seq) {
        VLOG(4) << "Checkpoint reply with seq=" << rSeq << " is already committed, ignoring!"
                << ". Current checkpoint seq is " << checkpoint.seq;
        return;
    }

    if (!checkpointCollectors_.hasCollector(round_, rSeq)) {
        VLOG(4) << "Checkpoint collector does not exist for seq=" << rSeq << " round=" << round_
                << " creating one now ";

        if (!checkpointCollectors_.initCollector(round_, rSeq, rSeq % snapshotInterval_ == 0)) {
            return;
        }
    }
    CheckpointCollector &coll = checkpointCollectors_.at(round_, rSeq);

    if (coll.commitReady()) {
        VLOG(4) << "COMMIT already sent for seq=" << rSeq << " round=" << round_ << " ignoring";
        return;
    }

    if (coll.addAndCheckReply(reply, sig)) {
        assert(reply.round() == round_);

        proto::Cert cert;
        coll.getCert(cert);

        if (!log_->addCert(rSeq, cert)) {
            VLOG(2) << "CHECKPOINT: Failed to add cert for seq=" << rSeq;
            return;
        }

        if (coll.commitReady()) {
            Commit commit;
            coll.getOwnCommit(commit);

            VLOG(3) << "Sending own COMMIT seq=" << commit.seq() << " round=" << commit.round()
                    << " digest=" << digest_to_hex(commit.log_digest())
                    << " app_digest=" << digest_to_hex(commit.app_digest());

            broadcastToReplicas(commit, MessageType::COMMIT);
        }

        return;
    }
}

void Replica::processSnapshot(const AppSnapshot &snapshot, uint32_t round)
{
    VLOG(4) << "CHECKPOINT Processing snapshot for seq=" << snapshot.seq << " round=" << round;

    if (!checkpointCollectors_.hasCollector(round, snapshot.seq)) {
        VLOG(4) << "CHECKPOINT Collector does not exist for round=" << round << " seq = " << snapshot.seq
                << " which means this was either committed or skipped";
        return;
    }

    CheckpointCollector &coll = checkpointCollectors_.at(round, snapshot.seq);
    coll.addOwnSnapshot(snapshot);

    if (coll.commitReady()) {
        Commit commit;
        coll.getOwnCommit(commit);

        VLOG(3) << "CHECKPOINT Sending own COMMIT seq=" << commit.seq() << " round=" << commit.round()
                << " digest=" << digest_to_hex(commit.log_digest())
                << " app_digest=" << digest_to_hex(commit.app_digest());

        broadcastToReplicas(commit, MessageType::COMMIT);
    }
}

void Replica::startCheckpoint(bool createSnapshot)
{
    uint32_t seq = log_->getNextSeq() - 1;

    if (createSnapshot) {
        uint32_t round = round_;
        app_->takeSnapshot([&, round](const AppSnapshot &snapshot) {
            // Queue to be processed by main thread
            snapshotQueue_.enqueue({round, snapshot});
        });
    }

    if (checkpointCollectors_.hasCollector(round_, seq)) {
        VLOG(4) << "Checkpoint collector already exists for seq=" << seq << " round=" << round_
                << " since we received messages from other replicas";
    } else {
        checkpointCollectors_.initCollector(round_, seq, createSnapshot);
    }

    checkpointCollectors_.at(round_, seq).addOwnState(log_->getDigest(seq), log_->getClientRecord());

    Reply reply;
    const ::LogEntry &entry = log_->getEntry(seq);   // TODO better namespace

    reply.set_client_id(entry.client_id);
    reply.set_client_seq(entry.client_seq);
    reply.set_replica_id(replicaId_);
    reply.set_round(round_);
    reply.set_seq(entry.seq);
    reply.set_digest(entry.digest);

    VLOG(1) << "PERF event=checkpoint_start seq=" << seq << " createSnapshot=" << createSnapshot << " round=" << round_
            << " log_digest=" << digest_to_hex(log_->getDigest());

    broadcastToReplicas(reply, MessageType::REPLY);
}

void Replica::sendSnapshotRequest(uint32_t replicaId, uint32_t targetSeq)
{
    SnapshotRequest snapshotRequest;
    snapshotRequest.set_round(round_);
    snapshotRequest.set_seq(targetSeq);
    snapshotRequest.set_replica_id(replicaId_);
    snapshotRequest.set_last_checkpoint_seq(log_->getStableCheckpoint().seq);

    VLOG(1) << "REPAIR Sending SNAPSHOT_REQUEST to " << replicaId << " for seq=" << targetSeq << " round=" << round_
            << " last_checkpoint_seq=" << log_->getStableCheckpoint().seq;

    sendMsgToDst(snapshotRequest, MessageType::SNAPSHOT_REQUEST, replicaAddrs_[replicaId]);
}

template <typename T> void Replica::sendMsgToDst(const T &msg, MessageType type, const Address &dst)
{
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
#if FABRIC_CRYPTO
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif
    endpoint_->SendPreparedMsgTo(dst, hdr);
}

template <typename T> void Replica::broadcastToReplicas(const T &msg, MessageType type)
{
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
#if FABRIC_CRYPTO
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif

    for (uint32_t i = 0; i < replicaAddrs_.size(); i++) {
        if (i != replicaId_) {
            endpoint_->SendPreparedMsgTo(replicaAddrs_[i], hdr);
        }
    }
}

void Replica::processCommit(const dombft::proto::Commit &commit, std::span<byte> sig)
{
    uint32_t seq = commit.seq();
    VLOG(3) << "Processing COMMIT from replica " << commit.replica_id() << " for seq " << seq
            << " round=" << commit.round() << " digest=" << digest_to_hex(commit.log_digest())
            << " app_digest=" << digest_to_hex(commit.app_digest());

    if (!checkpointCollectors_.hasCollector(commit.round(), seq)) {
        VLOG(4) << "Checkpoint collector does not exist for seq=" << seq << " round=" << commit.round()
                << " creating one now";
        if (!checkpointCollectors_.initCollector(
                commit.round(), seq, seq % snapshotInterval_ == 0 || commit.has_app_digest()
            )) {
            return;
        }
    }
    CheckpointCollector &coll = checkpointCollectors_.at(commit.round(), seq);

    // add current commit msg to collector
    // use the majority agreed commit message if exists
    if (coll.addAndCheckCommit(commit, sig)) {
        // TODO we can update our round in case commit.round() > round_
        // if (round_ < commit.round()) {
        //     LOG(WARNING) << "Ignoring checkpoint for future round " << commit.round() << " current round is " <<
        //     round_; return;
        // }

        ::LogCheckpoint checkpoint;
        coll.getCheckpoint(checkpoint);
        uint32_t seq = checkpoint.seq;

        if (checkpointDropFreq_ && seq / checkpointInterval_ % checkpointDropFreq_ == 0) {
            LOG(INFO) << "Dropping checkpoint seq=" << seq;
            return;
        }

        LOG(INFO) << "Trying to commit  seq=" << seq << " commit_digest=" << digest_to_hex(checkpoint.logDigest)
                  << " in round=" << commit.round();

        if (seq >= log_->getNextSeq() || log_->getDigest(seq) != checkpoint.logDigest) {
            // TODO choose a random replica from those that have this
            assert(!checkpoint.commits.empty());
            uint32_t replicaId =
                std::next(checkpoint.commits.begin(), GetMicrosecondTimestamp() % checkpoint.commits.size())
                    ->second.replica_id();
            if (seq >= log_->getNextSeq()) {
                LOG(INFO) << "My log is behind round=" << round_ << " nextSeq=" << log_->getNextSeq();
            } else {
                LOG(INFO) << "My log digest " << digest_to_hex(log_->getDigest(seq))
                          << " does not match the commit message digest " << digest_to_hex(checkpoint.logDigest);
            }

            // sendSnapshotRequest(replicaId, checkpoint.seq);

            // This can cause replica to fall behind; by the time it gets a snapshot, it would already be too far behind
            if (!checkpointSnapshotRequested_) {
                sendSnapshotRequest(replicaId, checkpoint.seq);
            }
            checkpointSnapshotRequested_ = true;

        } else if (!coll.needsSnapshot()) {
            log_->setCheckpoint(checkpoint);

            VLOG(1) << "PERF event=checkpoint_end snapshot=false seq=" << seq
                    << " log_digest=" << digest_to_hex(checkpoint.logDigest)
                    << " app_digest=" << digest_to_hex(checkpoint.appDigest);

            // if there is overlapping and later checkpoint commits first, skip earlier ones
            checkpointCollectors_.cleanStaleCollectors(
                log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq
            );

        } else if (checkpoint.snapshot != nullptr) {
            log_->setCheckpoint(checkpoint);

            VLOG(1) << "PERF event=checkpoint_end snapshot=true seq=" << seq
                    << " log_digest=" << digest_to_hex(checkpoint.logDigest)
                    << " app_digest=" << digest_to_hex(checkpoint.appDigest);

            // if there is overlapping and later checkpoint commits first, skip earlier ones
            checkpointCollectors_.cleanStaleCollectors(
                log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq
            );
        } else {
            VLOG(4) << "CHECKPOINT: Quorum of commits match our log, but we do not have a snapshot yet!"
                    << " Will wait for our snapshot request to finish and then receive our own commit!";
            // TODO, if a replica needed a snapshot to catch up, this case may happen and then the
            // snapshot will never be taken. This would get cleaned up next time...
        }

        // Unregister repair timer set by client as checkpoint confirms progress
        repairTimeoutStart_ = 0;
    }
}

void Replica::processSnapshotRequest(const SnapshotRequest &request)
{
    uint32_t reqSeq = request.seq();
    uint32_t round = request.round();
    VLOG(1) << "Processing SNAPSHOT_REQUEST from replica " << request.replica_id() << " for seq " << reqSeq
            << " from sequnece " << request.last_checkpoint_seq();

    if (reqSeq > log_->getCommittedCheckpoint().seq) {
        VLOG(4) << "Requested req " << reqSeq << " is ahead of current checkpoint, cannot provide snapshot";
        return;
    }
    // In fact, returned state snapshot is always the latest snapshot (with the checkpoint.seq)
    // the LOG is just to make it clear.

    // TODO, use request's last checkpoint sequence to return a delta instead..

    SnapshotReply snapshotReply;
    snapshotReply.set_round(round_);
    snapshotReply.set_seq(log_->getCommittedCheckpoint().seq);
    snapshotReply.set_replica_id(replicaId_);

    const ::LogCheckpoint &committedCp = log_->getCommittedCheckpoint();
    committedCp.toProto(*snapshotReply.mutable_checkpoint());

    const ::LogCheckpoint &stableCp = log_->getStableCheckpoint();
    if (request.last_checkpoint_seq() < stableCp.seq) {
        VLOG(1) << "Snapshot request too far back, sending application snapshot!";

        assert(stableCp.snapshot != nullptr);

        snapshotReply.set_snapshot(*stableCp.snapshot);
        stableCp.toProto(*snapshotReply.mutable_snapshot_checkpoint());
    }

    // Adding requests not included in snapshot up to my committed_seq
    uint32_t startSeq = std::max(request.last_checkpoint_seq(), stableCp.seq) + 1;
    for (uint32_t seq = startSeq; seq <= log_->getCommittedCheckpoint().seq; seq++) {
        auto &entry = log_->getEntry(seq);

        dombft::proto::LogEntry entryProto;
        entry.toProto(entryProto);
        (*snapshotReply.add_log_entries()) = entryProto;
    }

    VLOG(1) << "Sending SNAPSHOT_REPLY to " << request.replica_id() << " round=" << snapshotReply.round() << " from "
            << startSeq << " to " << log_->getCommittedCheckpoint().seq
            << " (app snapshot=" << snapshotReply.has_snapshot() << ")";

    sendMsgToDst(snapshotReply, MessageType::SNAPSHOT_REPLY, replicaAddrs_[request.replica_id()]);
}

void Replica::processSnapshotReply(const dombft::proto::SnapshotReply &snapshotReply)
{
    if (snapshotReply.round() < round_) {
        VLOG(1) << "Snapshot reply round outdated, skipping";

        // TODO these are probably not necessary
        repairSnapshotRequested_ = false;
        checkpointSnapshotRequested_ = false;
        return;
    }
    if (snapshotReply.seq() <= log_->getCommittedCheckpoint().seq) {
        VLOG(1) << "Seq " << snapshotReply.seq() << " is already committed, skipping snapshot reply";
        repairSnapshotRequested_ = false;
        checkpointSnapshotRequested_ = false;

        return;
    }
    if (repair_) {

        // Finish applying LogSuffix computed from repair proposal and return to normal processing

        // This prevents us from accidentally ressettiting to some old state
        if (!repairSnapshotRequested_ && snapshotReply.round() <= round_) {
            LOG(ERROR) << "Received snapshot reply during repair before repair finishes due to previous checkpoint, "
                          "ignoring... !";
            return;
        }
        LogSuffix &logSuffix = getRepairLogSuffix();
        if (snapshotReply.seq() < logSuffix.checkpoint->seq()) {
            LOG(ERROR) << "Received snapshot reply during repair that is too old, ignoring... !"
                       << " Previous checkpoint for seq=" << snapshotReply.seq()
                       << " need seq=" << logSuffix.checkpoint->seq();
            return;
        }

        uint32_t startSeq = snapshotReply.log_entries().size() > 0 ? snapshotReply.log_entries(0).seq()
                                                                   : snapshotReply.snapshot_checkpoint().seq();

        LOG(INFO) << "Processing (during repair) SNAPSHOT_REPLY from replica " << snapshotReply.replica_id()
                  << " for seq " << snapshotReply.seq() << " with log from " << startSeq
                  << " has_snapshot=" << snapshotReply.has_snapshot();

        std::vector<::ClientRequest> abortedRequests = getAbortedEntries(logSuffix, log_, curRoundStartSeq_);

        if (!log_->resetToSnapshot(snapshotReply)) {
            // TODO handle this case properly by retrying on another replica
            LOG(ERROR) << "Failed to reset log to snapshot, snapshot did not match digest!";
            throw std::runtime_error("Snapshot digest mismatch");
        }

        // if there is overlapping and later checkpoint commits first, skip earlier ones
        checkpointCollectors_.cleanStaleCollectors(log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq);

        if (snapshotReply.checkpoint().seq() > logSuffix.checkpoint->seq()) {
            LOG(WARNING) << "Snapshot is from future: seq=" << snapshotReply.seq() << " round=" << snapshotReply.round()
                         << ". We requested seq=" << logSuffix.checkpoint->seq() << " round=" << round_
                         << " repair_last_seq=" << logSuffix.checkpoint->seq() + logSuffix.entries.size()
                         << ". Still applying it before finishRepair, but will likely drop lots of messages";
        }

        applySuffix(logSuffix, log_);
        finishRepair(abortedRequests);

        // TODO temporary fix for issue #120, this may lead to later issues though
        round_ = std::max(round_, snapshotReply.round());
    } else {
        // Apply snapshot from checkpoint and reorder my log
        // TODO make sure this isn't outdated...

        uint32_t startSeq = snapshotReply.log_entries().size() > 0 ? snapshotReply.log_entries(0).seq()
                                                                   : snapshotReply.snapshot_checkpoint().seq();

        LOG(INFO) << "Processing SNAPSHOT_REPLY from replica " << snapshotReply.replica_id() << " for seq "
                  << snapshotReply.seq() << " with log from " << startSeq
                  << " has_snapshot=" << snapshotReply.has_snapshot();

        if (!log_->applySnapshotModifyLog(snapshotReply)) {
            LOG(ERROR) << "Failed to apply snapshot because it did not match digest!";
            // TODO handle this better by requesting from another replica..
            throw std::runtime_error("Snapshot digest mismatch");
        }

        // TODO temporary fix for issue #120, this may lead to later issues though
        round_ = std::max(round_, snapshotReply.round());

        // if there is overlapping and later checkpoint commits first, skip earlier ones
        checkpointCollectors_.cleanStaleCollectors(log_->getStableCheckpoint().seq, log_->getCommittedCheckpoint().seq);

        // Resend replies after modifying log
        for (int seq = log_->getCommittedCheckpoint().seq + 1; seq < log_->getNextSeq(); seq++) {
            auto &entry = log_->getEntry(seq);

            Reply reply;

            reply.set_client_id(entry.client_id);
            reply.set_client_seq(entry.client_seq);
            reply.set_replica_id(replicaId_);
            reply.set_result(entry.result);
            reply.set_seq(seq);
            reply.set_round(round_);
            reply.set_digest(entry.digest);

            VLOG(2) << "PERF event=update_digest seq=" << seq << " digest=" << digest_to_hex(entry.digest)
                    << " c_id=" << entry.client_id << " c_seq=" << entry.client_seq;

            sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[entry.client_id]);
        }

        VLOG(1) << "PERF event=align checkpoint_seq=" << log_->getCommittedCheckpoint().seq
                << " log_seq=" << log_->getNextSeq() - 1 << " log_digest=" << digest_to_hex(log_->getDigest());
    }

    // Got the snapshot
    repairSnapshotRequested_ = false;
    checkpointSnapshotRequested_ = false;
}

void Replica::processRepairClientTimeout(const dombft::proto::RepairClientTimeout &msg, std::span<byte> sig)
{
    if (repair_) {
        VLOG(7) << "Received repair trigger during a repair from client " << msg.client_id()
                << " for cseq=" << msg.client_seq();
        return;
    }

    if (repairTimeoutStart_ != 0) {
        VLOG(2) << "Received redundant repair trigger due to client side timeout from client_id=" << msg.client_id()
                << " for cseq=" << msg.client_seq();
        return;
    }

    if (msg.round() != round_) {
        VLOG(2) << "Received repair trigger for round " << msg.round() << " != " << round_;
        return;
    }

    LOG(INFO) << "Received repair trigger from client_id=" << msg.client_id() << " for cseq=" << msg.client_seq();
    repairTimeoutStart_ = GetMicrosecondTimestamp();
}

void Replica::processRepairReplicaTimeout(const dombft::proto::RepairReplicaTimeout &msg, std::span<byte> sig)
{
    // Note assume msg is verfied here
    if (repair_) {
        VLOG(4) << "Received repair replica timeout during a repair from replica " << msg.replica_id();
        return;
    }

    VLOG(4) << "Received repair replica timeout from " << msg.replica_id() << " for round " << msg.round();

    uint32_t repId = msg.replica_id();

    repairReplicaTimeouts_[repId] = msg;
    repairReplicaTimeoutSigs_[repId] = std::string(sig.begin(), sig.end());

    dombft::proto::RepairTimeoutProof proof;
    for (auto &[repId, msg] : repairReplicaTimeouts_) {
        if (msg.round() != round_)
            continue;

        (*proof.add_timeouts()) = msg;
        proof.add_signatures(repairReplicaTimeoutSigs_[repId]);
    }

    if (proof.timeouts_size() == f_ + 1) {
        LOG(INFO) << "Gathered timeout proof for round " << round_ << ", starting repair and broadcasting!";
        broadcastToReplicas(proof, REPAIR_TIMEOUT_PROOF);
        startRepair();
    }
}

void Replica::processRepairReplyProof(const dombft::proto::RepairReplyProof &msg)
{
    // Ignore repeated repair triggers
    if (repair_) {
        VLOG(6) << "Received repair trigger during a repair from client " << msg.client_id()
                << " for cseq=" << msg.client_seq();
        return;
    }

    // Proof is verified by verify thread
    if (msg.round() < round_) {
        VLOG(6) << "Received repair trigger proof for previous round " << msg.round() << " < " << round_;
        return;
    }

    if (msg.round() > round_) {
        VLOG(6) << "Received repair trigger proof for round " << msg.round() << " > " << round_;
        return;
    }

    LOG(INFO) << "Repair trigger for round " << msg.round() << " client_id=" << msg.client_id()
              << " cseq=" << msg.client_seq() << " has a proof, starting repair!";

    // Print out proof

    std::ostringstream oss;
    oss << "round=" << round_ << "\n";
    for (int i = 0; i < msg.replies().size(); i++) {
        const auto &reply = msg.replies(i);
        oss << reply.replica_id() << " " << digest_to_hex(reply.digest()) << " " << reply.seq() << " " << reply.round()
            << "\n";
    }

    LOG(INFO) << "Repair proof:\n" << oss.str();

    // TODO skip sending to ourself, we implictly don't repeat processing this message because we ignore proofs
    // if we already are in fallback.
    broadcastToReplicas(msg, REPAIR_REPLY_PROOF);
    startRepair();
}

void Replica::processRepairTimeoutProof(const dombft::proto::RepairTimeoutProof &msg)
{
    // Ignore repeated repair triggers
    if (repair_) {
        VLOG(5) << "Received timeout proof after I already started repair for round " << round_;
        return;
    }

    // Proof is verified by verify thread
    if (msg.round() < round_) {
        LOG(INFO) << "Received repair timeout proof for previous round " << msg.round() << " < " << round_;
        return;
    }

    if (msg.round() > round_) {
        VLOG(6) << "Received repair trigger proof for future round " << msg.round() << " > " << round_;
        return;
    }

    LOG(INFO) << "Received repair timeout proof, starting repair!";

    // TODO skip sending to ourself, we implictly don't repeat processing this message because we ignore proofs
    // if we already are in fallback.
    broadcastToReplicas(msg, REPAIR_TIMEOUT_PROOF);
    startRepair();
}

void Replica::processRepairStart(const RepairStart &msg, std::span<byte> sig)
{
    if ((msg.pbft_view() % replicaAddrs_.size()) != replicaId_) {
        LOG(INFO) << "Received REPAIR_START for round " << msg.round() << " pbft_view " << msg.pbft_view()
                  << " where I am not proposer";
        return;
    }

    uint32_t repId = msg.replica_id();
    uint32_t repRound = msg.round();
    if (msg.round() < round_) {
        LOG(INFO) << "Received REPAIR_START for round " << msg.round() << " from replica " << repId << " while own is "
                  << round_;
        return;
    }

    // A corner case where (older round + pbft_view) targets the same primary and overwrite the newer ones
    if (repairHistorys_.count(repId) && repairHistorys_[repId].round() >= repRound) {
        LOG(INFO) << "Received REPAIR_START for round " << repRound << " pbft_view " << msg.pbft_view()
                  << " from replica " << repId << " which is outdated";
        return;
    }
    repairHistorys_[repId] = msg;
    repairHistorySigs_[repId] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received repairStart message from replica " << repId;

    if (!isPrimary()) {
        return;
    }

    // First check if we have 2f + 1 repair start messages for the same round
    auto numStartMsgs = std::count_if(repairHistorys_.begin(), repairHistorys_.end(), [&](auto &startMsg) {
        return startMsg.second.round() == repRound;
    });

    if (numStartMsgs == quorumSize_) {

        VLOG(1) << "PERF event=repair_proposal replica_id=" << replicaId_ << " round=" << round_
                << " pbft_view=" << pbftView_;

        doPrePreparePhase(repRound);
    }
}

void Replica::processRepairDone(const RepairDone &msg)
{
    if (msg.round() == round_ && repair_) {
        LOG(ERROR) << "Processing RepairDone for round=" << round_ << " not implemented!";
        return;
    }
    // TODO  finish implementing allowing replica to catch up with a RepairDone
}

LogSuffix &Replica::getRepairLogSuffix()
{
    // This is just to cache the processing of the repairProposal
    // TODO make sure this works during view change as well.
    if (!repairProposalLogSuffix_.has_value() || repairProposalLogSuffix_.value().round != round_) {
        repairProposalLogSuffix_ = LogSuffix();
        repairProposalLogSuffix_->replicaId = replicaId_;
        repairProposalLogSuffix_->round = round_;
        getLogSuffixFromProposal(repairProposal_.value(), repairProposalLogSuffix_.value());
    }
    return repairProposalLogSuffix_.value();
}

bool Replica::verifyCert(const Cert &cert)
{
    if (cert.replies().size() < quorumSize_) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_
                  << " quorumSize_=" << quorumSize_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to " << "cert signatures size"
                  << cert.signatures().size();
        return false;
    }

    // check if the replies are matching and no duplicate replies from same replica
    std::map<ReplyKey, std::unordered_set<uint32_t>> matchingReplies;
    // Verify each signature in the cert
    for (int i = 0; i < cert.replies().size(); i++) {
        const Reply &reply = cert.replies()[i];
        const std::string &sig = cert.signatures()[i];
        uint32_t replicaId = reply.replica_id();

        ReplyKey key = {reply.seq(),        reply.round(),  reply.client_id(),
                        reply.client_seq(), reply.digest(), reply.result()};
        matchingReplies[key].insert(replicaId);

        std::string serializedReply = reply.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, reply.replica_id()}
            )) {
            LOG(INFO) << "Cert failed to verify!";
            return false;
        }
    }
    if (matchingReplies.size() > 1) {
        LOG(WARNING) << "Cert has non-matching replies!";
        return false;
    }
    if (matchingReplies.begin()->second.size() < cert.replies().size()) {
        LOG(WARNING) << "Cert has replies from the same replica!";
        return false;
    }

    return true;
}

bool Replica::verifyRepairReplyProof(const RepairReplyProof &proof)
{
    if (proof.replies().size() < f_ + 1) {
        VLOG(1) << "Repair reply proof has only " << proof.replies().size() << " replies, need " << f_ + 1;
        return false;
    }

    if (proof.replies().size() != proof.signatures().size()) {
        LOG(INFO) << "Repair reply proof has " << proof.replies().size() << " replies but " << proof.signatures().size()
                  << " signatures";
        return false;
    }

    std::unordered_set<uint32_t> replicaIds;

    for (int i = 0; i < proof.replies().size(); i++) {
        const Reply &reply = proof.replies()[i];
        const std::string &sig = proof.signatures()[i];

        if (replicaIds.contains(reply.replica_id())) {
            LOG(INFO) << "Repair reply proof has duplicate replica " << reply.replica_id();
            return false;
        }
        replicaIds.insert(reply.replica_id());

        std::string serializedReply = reply.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, reply.replica_id()}
            )) {
            LOG(INFO) << "Repair reply proof signature failed to verify for replica " << reply.replica_id();
            return false;
        }
    }

    return true;
}

bool Replica::verifyRepairTimeoutProof(const RepairTimeoutProof &proof)
{
    if (proof.timeouts().size() < f_ + 1) {
        VLOG(1) << "Repair timeout proof has only " << proof.timeouts().size() << " timeouts, need " << f_ + 1;
        return false;
    }

    if (proof.timeouts().size() != proof.signatures().size()) {
        LOG(INFO) << "Repair timeout proof has " << proof.timeouts().size() << " timeouts but "
                  << proof.signatures().size() << " signatures";
        return false;
    }

    std::unordered_set<uint32_t> replicaIds;
    uint32_t round = proof.timeouts(0).round();

    for (int i = 0; i < proof.timeouts().size(); i++) {
        const RepairReplicaTimeout &timeout = proof.timeouts(i);
        const std::string &sig = proof.signatures(i);

        if (timeout.round() != round) {
            LOG(INFO) << "Repair timeout proof has different rounds: " << timeout.round() << " vs " << round;
            return false;
        }

        if (replicaIds.contains(timeout.replica_id())) {
            LOG(INFO) << "Repair timeout proof has duplicate replica " << timeout.replica_id();
            return false;
        }
        replicaIds.insert(timeout.replica_id());

        std::string serializedTimeout = timeout.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedTimeout.c_str(), serializedTimeout.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, timeout.replica_id()}
            )) {
            LOG(INFO) << "Repair timeout proof signature failed to verify for replica " << timeout.replica_id();
            return false;
        }
    }

    return true;
}

bool Replica::verifyCheckpoint(const LogCheckpoint &checkpoint)
{
    // Basic verification of checkpoint structure
    if (checkpoint.seq() < 0) {
        LOG(INFO) << "Invalid checkpoint sequence: " << checkpoint.seq();
        return false;
    }

    if (checkpoint.log_digest().empty() || checkpoint.app_digest().empty()) {
        LOG(INFO) << "Checkpoint missing required digests";
        return false;
    }

    // TODO: Add more sophisticated verification logic if needed
    return true;
}

bool Replica::verifyRepairLog(const RepairStart &log)
{
    // Basic verification of repair log structure
    if (log.round() < 0 || log.pbft_view() < 0) {
        LOG(INFO) << "Invalid repair log round or view";
        return false;
    }

    // TODO: Add more verification logic
    return true;
}

bool Replica::verifyRepairProposal(const RepairProposal &proposal)
{
    if (proposal.logs().size() < quorumSize_) {
        LOG(INFO) << "Repair proposal has insufficient logs: " << proposal.logs().size() << " < " << quorumSize_;
        return false;
    }

    if (proposal.logs().size() != proposal.signatures().size()) {
        LOG(INFO) << "Repair proposal logs and signatures size mismatch";
        return false;
    }

    // Verify all signatures
    for (int i = 0; i < proposal.logs().size(); i++) {
        const RepairStart &log = proposal.logs(i);
        const std::string &sig = proposal.signatures(i);

        if (!verifyRepairLog(log)) {
            return false;
        }

        std::string serializedLog = log.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedLog.c_str(), serializedLog.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, log.replica_id()}
            )) {
            LOG(INFO) << "Repair proposal signature failed to verify for replica " << log.replica_id();
            return false;
        }
    }

    return true;
}

bool Replica::verifyViewChange(const PBFTViewChange &viewChange)
{
    // Basic verification of view change structure
    if (viewChange.pbft_view() < 0 || viewChange.round() < 0) {
        LOG(INFO) << "Invalid view change view or round";
        return false;
    }

    if (viewChange.prepares().size() != viewChange.prepare_sigs().size()) {
        LOG(INFO) << "View change prepares and signatures size mismatch";
        return false;
    }

    // Verify prepare signatures if present
    for (int i = 0; i < viewChange.prepares().size(); i++) {
        const PBFTPrepare &prepare = viewChange.prepares(i);
        const std::string &sig = viewChange.prepare_sigs(i);

        std::string serializedPrepare = prepare.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedPrepare.c_str(), serializedPrepare.size(), (byte *) sig.c_str(), sig.size(),
                {NodeType::REPLICA, prepare.replica_id()}
            )) {
            LOG(INFO) << "View change prepare signature failed to verify for replica " << prepare.replica_id();
            return false;
        }
    }

    return true;
}

bool Replica::verifyRepairDone(const RepairDone &done)
{
    // Basic verification of repair done structure
    if (done.round() < 0) {
        LOG(INFO) << "Invalid repair done round";
        return false;
    }

    // TODO: Add more verification logic
    return true;
}

void Replica::startRepair()
{
    assert(!repair_);
    repair_ = true;
    repairViewStart_ = GetMicrosecondTimestamp();

    VLOG(1) << "PERF event=repair_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << round_ << " pbft_view=" << pbftView_;

    LOG(INFO) << "Starting repair for round=" << round_ << " pbft_view=" << pbftView_;

    RepairStart repairStart;
    repairStart.set_replica_id(replicaId_);
    repairStart.set_round(round_);
    repairStart.set_pbft_view(pbftView_);

    // Add current log state
    const ::LogCheckpoint &committedCp = log_->getCommittedCheckpoint();
    committedCp.toProto(*repairStart.mutable_checkpoint());

    // Add any uncommitted entries
    for (uint32_t seq = committedCp.seq + 1; seq < log_->getNextSeq(); seq++) {
        const ::LogEntry &entry = log_->getEntry(seq);
        dombft::proto::LogEntry *protoEntry = repairStart.add_log_entries();
        entry.toProto(*protoEntry);
    }

    broadcastToReplicas(repairStart, REPAIR_START);
}

void Replica::finishRepair(const std::vector<::ClientRequest> &abortedReqs)
{
    repair_ = false;
    repairViewStart_ = 0;

    VLOG(1) << "PERF event=repair_end replica_id=" << replicaId_ << " seq=" << log_->getNextSeq() << " round=" << round_
            << " pbft_view=" << pbftView_;

    LOG(INFO) << "Finished repair for round=" << round_ << " pbft_view=" << pbftView_;

    // Process any queued requests that came in during repair
    for (auto &[key, req] : repairQueuedReqs_) {
        processClientRequest(req, true);
    }
    repairQueuedReqs_.clear();

    // Send repair summary to clients about aborted requests
    if (!abortedReqs.empty()) {
        sendRepairSummaryToClients();
    }

    // Clean up repair state
    repairReplicaTimeouts_.clear();
    repairReplicaTimeoutSigs_.clear();
    repairHistorys_.clear();
    repairHistorySigs_.clear();
    repairProposal_.reset();
    repairProposalLogSuffix_.reset();
    pbftState_ = PBFTState();
}

void Replica::tryFinishRepair()
{
    if (!repair_) {
        return;
    }

    // Simple implementation - just finish repair for now
    finishRepair({});
}

void Replica::sendRepairSummaryToClients()
{
    // Send repair summary notifications to clients
    // This is a simplified implementation
    LOG(INFO) << "Sending repair summary to clients for round=" << round_;
}

std::string Replica::getProposalDigest(const dombft::proto::RepairProposal &proposal)
{
    // use signatures as digest, since signatures are can function as the the digest of each proposal
    CryptoPP::SHA256 hash;
    byte digestBuf[CryptoPP::SHA256::DIGESTSIZE];
    std::string digestStr;

    for (const auto &sig : proposal.signatures()) {
        digestStr += sig;
    }

    hash.Update(reinterpret_cast<const byte *>(digestStr.data()), digestStr.size());
    hash.Final(digestBuf);

    return std::string(reinterpret_cast<const char *>(digestBuf), CryptoPP::SHA256::DIGESTSIZE);
}

void Replica::startViewChange()
{
    // TODO VC: If view change was already true, double timeout here

    pbftView_++;
    repair_ = true;
    viewChange_ = true;
    viewPrepared_ = false;
    VLOG(1) << "PERF event=viewchange_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << round_ << " pbft_view=" << pbftView_;
    LOG(INFO) << "Starting ViewChange on round " << round_ << " pbft_view " << pbftView_;

    PBFTViewChange viewChange;
    viewChange.set_replica_id(replicaId_);
    viewChange.set_round(preparedRound_);
    viewChange.set_pbft_view(pbftView_);

    // Add the latest quorum of prepares and sigs
    if (preparedRound_ != UINT32_MAX) {
        for (const auto &[repId, prepare] : pbftState_.prepares) {
            *(viewChange.add_prepares()) = prepare;
            *(viewChange.add_prepare_sigs()) = pbftState_.prepareSigs[repId];
        }
        viewChange.set_proposal_digest(pbftState_.proposalDigest);
        viewChange.mutable_proposal()->CopyFrom(pbftState_.proposal);
    }

    broadcastToReplicas(viewChange, PBFT_VIEWCHANGE);
}

void Replica::doPrePreparePhase(uint32_t round)
{
    if (!isPrimary()) {
        LOG(ERROR) << "Attempted to doPrePrepare from non-primary replica!";
        return;
    }
    LOG(INFO) << "PrePrepare for pbft_view=" << pbftView_ << " in primary replicaId=" << replicaId_;

    PBFTPrePrepare prePrepare;
    prePrepare.set_primary_id(replicaId_);
    prePrepare.set_round(round);
    prePrepare.set_pbft_view(pbftView_);
    // Piggyback the repair proposal
    RepairProposal *proposal = prePrepare.mutable_proposal();
    proposal->set_replica_id(replicaId_);
    proposal->set_round(round);
    for (auto &startMsg : repairHistorys_) {
        if (startMsg.second.round() != round)
            continue;

        *(proposal->add_logs()) = startMsg.second;
        *(proposal->add_signatures()) = repairHistorySigs_[startMsg.first];
    }

    proposalDigest_ = getProposalDigest(prePrepare.proposal());
    prePrepare.set_proposal_digest(proposalDigest_);
    broadcastToReplicas(prePrepare, PBFT_PREPREPARE);
}

void Replica::doPreparePhase()
{
    LOG(INFO) << "Prepare for round=" << repairProposal_->round() << " replicaId=" << replicaId_;
    PBFTPrepare prepare;
    prepare.set_replica_id(replicaId_);
    prepare.set_round(repairProposal_->round());
    prepare.set_pbft_view(pbftView_);
    prepare.set_proposal_digest(proposalDigest_);

    broadcastToReplicas(prepare, PBFT_PREPARE);
}

void Replica::doCommitPhase()
{
    LOG(INFO) << "Commit for round=" << repairProposal_->round() << " replicaId=" << replicaId_;
    PBFTCommit commit;
    commit.set_replica_id(replicaId_);
    commit.set_round(repairProposal_->round());
    commit.set_pbft_view(pbftView_);
    commit.set_proposal_digest(proposalDigest_);

    broadcastToReplicas(commit, PBFT_COMMIT);
}

void Replica::processPrePrepare(const PBFTPrePrepare &msg)
{
    if (msg.round() < round_) {
        LOG(INFO) << "Received old repair preprepare from round=" << msg.round() << " own round is " << round_;
        return;
    }

    if (msg.round() > round_) {
        LOG(INFO) << "Received future repair preprepare from round=" << msg.round() << " own round is " << round_;
        return;
    }

    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received preprepare from replicaId=" << msg.primary_id() << " for round=" << msg.round()
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (getPrimary() != msg.primary_id()) {
        LOG(INFO) << "Received repair preprepare from non-primary replica " << msg.primary_id()
                  << ". Current selected primary is " << getPrimary();
        return;
    }

    LOG(INFO) << "PrePrepare RECEIVED for round=" << msg.round() << " from replicaId=" << msg.primary_id();
    VLOG(1) << "PERF event=repair_preprepare replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << preparedRound_ << " pbft_view=" << pbftView_
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    // accepts the proposal as long as it's from the primary
    repairProposal_ = msg.proposal();
    proposalDigest_ = msg.proposal_digest();

    if (viewChangeByPrepare()) {
        holdPrepareOrCommit_ = !holdPrepareOrCommit_;
        LOG(INFO) << "Prepare message held to cause timeout in prepare phase for view change";
        return;
    }
    doPreparePhase();
}

void Replica::processPrepare(const PBFTPrepare &msg, std::span<byte> sig)
{
    uint32_t inRound = msg.round();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received prepare from replicaId=" << msg.replica_id() << " for round=" << inRound
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inRound < round_ && viewPrepared_) {
        LOG(INFO) << "Received old repair prepare from round=" << inRound << " own round is " << round_;
        return;
    }

    if (inRound > round_) {
        LOG(INFO) << "Received future repair prepare from round=" << inRound << " own round is " << round_;
        return;
    }

    if (repairPrepares_.count(msg.replica_id()) && repairPrepares_[msg.replica_id()].round() > inRound &&
        repairPrepares_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old prepare received from replicaId=" << msg.replica_id() << " for round=" << inRound;
        return;
    }
    repairPrepares_[msg.replica_id()] = msg;
    repairPrepareSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());
    LOG(INFO) << "Prepare RECEIVED for round=" << inRound << " from replicaId=" << msg.replica_id();
    // skip if already prepared for it
    // note: if viewPrepared_==false, then viewChange_==true
    if (viewPrepared_ && preparedRound_ == inRound) {
        LOG(INFO) << "Already prepared for round=" << inRound << " pbft_view=" << pbftView_;
        return;
    }
    if (!repairProposal_.has_value() || repairProposal_.value().round() < inRound) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process prepare";
        return;
    }
    assert(repairProposal_.has_value());
    // Make sure the Prepare msgs are for the corresponding PrePrepare msg
    auto numMsgs = std::count_if(repairPrepares_.begin(), repairPrepares_.end(), [this](auto &curMsg) {
        return curMsg.second.round() == repairProposal_.value().round() &&
               curMsg.second.proposal_digest() == proposalDigest_ && curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < quorumSize_) {
        LOG(INFO) << "Prepare received from " << numMsgs << " replicas, waiting for 2f + 1 to proceed";
        return;
    }
    // Store PBFT states for potential view change
    preparedRound_ = repairProposal_.value().round();
    viewPrepared_ = true;
    pbftState_.proposal = repairProposal_.value();
    pbftState_.proposalDigest = proposalDigest_;
    pbftState_.prepares.clear();
    for (const auto &[repId, prepare] : repairPrepares_) {
        if (prepare.round() == preparedRound_) {
            pbftState_.prepares[repId] = prepare;
            pbftState_.prepareSigs[repId] = repairPrepareSigs_[repId];
        }
    }
    LOG(INFO) << "Prepare received from 2f + 1 replicas, agreement reached for round=" << preparedRound_
              << " pbft_view=" << pbftView_;

    VLOG(1) << "PERF event=prepared replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << preparedRound_ << " pbft_view=" << pbftView_
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    if (viewChangeByCommit()) {
        if (commitLocalInViewChange_) {
            LOG(INFO) << "Commit message only send to itself to commit locally to advance to next round";
            doCommitPhase();
        } else {
            LOG(INFO) << "Commit message held to cause timeout in commit phase for view change";
        }
        return;
    }
    doCommitPhase();
}

void Replica::processPBFTCommit(const PBFTCommit &msg, std::span<byte> sig)
{
    uint32_t inInst = msg.round();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received commit from replicaId=" << msg.replica_id() << " for round=" << inInst
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inInst < round_ && !viewChange_) {
        LOG(INFO) << "Received old repair commit from round=" << inInst << " own round is " << round_;
        return;
    }

    if (inInst > round_) {
        LOG(INFO) << "Received future repair commit from round=" << inInst << " own round is " << round_;
        return;
    }

    if (repairPBFTCommits_.count(msg.replica_id()) && repairPBFTCommits_[msg.replica_id()].round() > inInst &&
        repairPBFTCommits_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old commit received from replicaId=" << msg.replica_id() << " for round=" << inInst;
        return;
    }
    repairPBFTCommits_[msg.replica_id()] = msg;
    repairCommitSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "PBFTCommit RECEIVED for round=" << inInst << " from replicaId=" << msg.replica_id();

    if (!repairProposal_.has_value() || repairProposal_.value().round() < inInst) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process commit";
        return;
    }

    if (preparedRound_ == UINT32_MAX || preparedRound_ != inInst || !viewPrepared_) {
        LOG(INFO) << "Not prepared for it, skipping commit!";
        // TODO get the proposal from another replica...
        return;
    }
    auto numMsgs = std::count_if(repairPBFTCommits_.begin(), repairPBFTCommits_.end(), [this](auto &curMsg) {
        return curMsg.second.round() == preparedRound_ && curMsg.second.proposal_digest() == proposalDigest_ &&
               curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < quorumSize_) {
        return;
    }

    VLOG(1) << "PERF event=repair_commit replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " round=" << preparedRound_ << " pbft_view=" << pbftView_
            << " log_digest=" << digest_to_hex(msg.log_digest())
            << " proposal_digest=" << digest_to_hex(msg.proposal_digest());

    LOG(INFO) << "Commit received from 2f + 1 replicas, Committed!";
    tryFinishRepair();
}

void Replica::processPBFTViewChange(const PBFTViewChange &msg, std::span<byte> sig)
{
    // No round checking as view change is not about round
    uint32_t inViewNum = msg.pbft_view();
    if (inViewNum < pbftView_) {
        LOG(INFO) << "Received outdated view change from pbft_view=" << inViewNum << " own pbft_view is " << pbftView_;
        return;
    }

    if (pbftViewChanges_.count(msg.replica_id()) && pbftViewChanges_[msg.replica_id()].pbft_view() > inViewNum) {
        LOG(INFO) << "Outdated view change received from replicaId=" << msg.replica_id()
                  << " for pbft_view=" << inViewNum;
        return;
    }

    pbftViewChanges_[msg.replica_id()] = msg;
    pbftViewChangeSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "ViewChange RECEIVED for pbft_view=" << inViewNum << " from replicaId=" << msg.replica_id();

    auto numMsgs = std::count_if(pbftViewChanges_.begin(), pbftViewChanges_.end(), [this, inViewNum](auto &curMsg) {
        return curMsg.second.pbft_view() == inViewNum;
    });

    if (numMsgs != quorumSize_) {
        return;
    }
    LOG(INFO) << "ViewChange for view " << inViewNum << " received from 2f + 1 replicas!";

    // non-primary replicas collect view change msgs for
    // 1. delaying timer setting for better liveness (avoid frequent view changes)
    // 2. check if the majority has a larger view# and start view change if so (avoid starting view change too late)

    repairViewStart_ = GetMicrosecondTimestamp();   // reset view change timeout (1) above

    // TODO VC: should do this if there are f + 1 VC messages here.
    // They also don't need to necessarily be in the same view
    if (inViewNum > pbftView_) {
        LOG(INFO) << "Majority has a larger view number, starting a new view change for the major view";
        pbftView_ = inViewNum - 1;   // will add 1 back in startViewChange
        startViewChange();
    }

    if (!isPrimary()) {
        pbftViewChanges_.clear();
        pbftViewChangeSigs_.clear();
        return;
    }

    // primary search for the latest prepared round
    uint32_t maxRound = UINT32_MAX;
    // vc.round can be UINT32_MAX if no prepares, which is fine
    for (const auto &[_, vc] : pbftViewChanges_) {
        if (vc.pbft_view() != inViewNum) {
            continue;
        }
        if (maxRound == UINT32_MAX && vc.round() < maxRound) {
            maxRound = vc.round();
        } else if (maxRound != UINT32_MAX && vc.round() > maxRound) {
            maxRound = vc.round();
        }
    }
    LOG(INFO) << "Found the latest prepared round=" << maxRound << " for pbft_view=" << inViewNum;

    PBFTNewView newView;
    newView.set_primary_id(replicaId_);
    newView.set_pbft_view(inViewNum);
    newView.set_round(maxRound);

    for (const auto &[repId, vc] : pbftViewChanges_) {
        newView.add_view_changes()->CopyFrom(vc);
        *(newView.add_view_change_sigs()) = pbftViewChangeSigs_[repId];
    }

    broadcastToReplicas(newView, PBFT_NEWVIEW);
}

void Replica::processPBFTNewView(const PBFTNewView &msg)
{
    // TODO(Hao) here should be some checks for view change msgs in NewView, skip for now..
    if (pbftView_ > msg.pbft_view()) {
        LOG(INFO) << "Received outdated new view from pbft_view=" << msg.pbft_view() << " own pbft_view is "
                  << pbftView_;
        return;
    }

    const auto &viewChanges = msg.view_changes();
    std::unordered_map<uint32_t, uint32_t> views;
    PBFTViewChange maxVC;
    for (const auto &vc : viewChanges) {
        views[vc.pbft_view()]++;
        if (vc.round() > maxVC.round()) {
            maxVC = vc;
        }
    }
    if (maxVC.pbft_view() != msg.pbft_view() || maxVC.round() != msg.round()) {
        LOG(INFO) << "Replica obtains a different choice of round=" << maxVC.round() << "," << msg.round()
                  << " and pbft_view=" << maxVC.pbft_view() << ", " << msg.pbft_view();
    }
    if (views[maxVC.pbft_view()] < quorumSize_) {
        LOG(INFO) << "The view number " << maxVC.pbft_view() << " does not have a 2f + 1 quorum";
        return;
    }
    LOG(INFO) << "Received NewView for pbft_view=" << msg.pbft_view() << " with prepared round=" << msg.round();
    if (pbftView_ == msg.pbft_view() && viewPrepared_) {
        LOG(INFO) << "Already in the same view and prepared for it, skip the new view message";
        return;
    }
    pbftView_ = msg.pbft_view();
    // in case it is not in view change already. Not quite sure this is correct way tho
    if (!viewChange_) {
        viewChange_ = true;
        assert(viewChange_);
        repair_ = true;
        viewPrepared_ = false;
        repairProposal_.reset();
        repairPrepares_.clear();
        repairPBFTCommits_.clear();
    }

    // TODO(Hao): test this corner case later
    if (msg.round() == UINT32_MAX) {
        LOG(INFO) << "No previously prepared round in new view, go back to normal state. EXIT FOR NOW";
        // assert(msg.round() != UINT32_MAX);
        // return;
    }
    repairProposal_ = maxVC.proposal();
    proposalDigest_ = maxVC.proposal_digest();
    // set view change param to true to bypass round check.
    doPreparePhase();
}

}   // namespace dombft