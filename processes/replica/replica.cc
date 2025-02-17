#include "replica.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/apps/kv_store.h"
#include "lib/common.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <openssl/pem.h>
#include <sstream>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(
    const ProcessConfig &config, uint32_t replicaId, uint32_t swapFreq, uint32_t viewChangeFreq,
    bool commitLocalInViewChange, uint32_t viewChangeNum, uint32_t checkpointDropFreq
)
    : replicaId_(replicaId)
    , f_(config.replicaIps.size() / 3)
    , numVerifyThreads_(config.replicaNumVerifyThreads)
    , fallbackTimeout_(config.replicaFallbackTimeout)
    , viewChangeTimeout_(config.replicaFallbackStartTimeout)
    , sigProvider_()
    , sendThreadpool_(config.replicaNumSendThreads)
    , instance_(0)
    , checkpointCollector_(replicaId_, f_)
    , swapFreq_(swapFreq)
    , checkpointDropFreq_(checkpointDropFreq)
    , viewChangeFreq_(viewChangeFreq)
    , viewChangeInst_(viewChangeFreq_)
    , commitLocalInViewChange_(commitLocalInViewChange)
    , viewChangeNum_(viewChangeNum)
{
    // TODO check for config errors
    std::string replicaIp = config.replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = config.receiverLocal ? "0.0.0.0" : replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = config.replicaPort;
    LOG(INFO) << "replicaPort=" << replicaPort;

    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".der";
    LOG(INFO) << "Loading key from " << replicaKey;
    if (!sigProvider_.loadPrivateKey(replicaKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    LOG(INFO) << "private key loaded";

    if (!sigProvider_.loadPublicKeys("client", config.clientKeysDir)) {
        LOG(ERROR) << "Unable to load client public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("receiver", config.receiverKeysDir)) {
        LOG(ERROR) << "Unable to load receiver public keys!";
        exit(1);
    }

    if (!sigProvider_.loadPublicKeys("replica", config.replicaKeysDir)) {
        LOG(ERROR) << "Unable to load receiver public keys!";
        exit(1);
    }

    LOG(INFO) << "instantiating log";

    if (config.app == AppType::COUNTER) {
        app_ = std::make_shared<Counter>();
    } else if (config.app == AppType::KV_STORE) {
        app_ = std::make_shared<KVStore>();
    } else {
        LOG(ERROR) << "Unrecognized App Type";
        exit(1);
    }
    log_ = std::make_shared<Log>(app_);
    LOG(INFO) << "log instantiated";

    if (config.transport == "nng") {
        auto addrPairs = getReplicaAddrs(config, replicaId_);

        size_t nClients = config.clientIps.size();
        for (size_t i = 0; i < nClients; i++) {
            // LOG(INFO) << "Client " << i << ": " << addrPairs[i].second.ip();
            clientAddrs_.push_back(addrPairs[i].second);
        }

        receiverAddr_ = addrPairs[nClients].second;

        for (size_t i = nClients + 1; i < addrPairs.size(); i++) {
            replicaAddrs_.push_back(addrPairs[i].second);
        }

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true, replicaAddrs_[replicaId]);
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
}

Replica::~Replica()
{
    // TODO cleanup... though we don't really reuse this
}

void Replica::run()
{
    // Submit first request
    LOG(INFO) << "Starting " << numVerifyThreads_ << " verify threads";
    running_ = true;
    for (uint32_t i = 0; i < numVerifyThreads_; i++) {
        verifyThreads_.emplace_back(&Replica::verifyMessagesThd, this);
    }

    LOG(INFO) << "Starting process thread";
    processThread_ = std::thread(&Replica::processMessagesThd, this);

    LOG(INFO) << "Starting main event loop...";
    endpoint_->LoopRun();
    LOG(INFO) << "Finishing main event loop...";

    for (std::thread &thd : verifyThreads_) {
        thd.join();
    }
    processThread_.join();
}

void Replica::handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender)
{
    // First make sure message is well formed

    // We skip verification of our own messages, and any message from the receiver
    // process (which does its own verification)
    byte *rawMsg = (byte *) msgHdr;
    std::vector<byte> msg(rawMsg, rawMsg + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen);

    if (*sender == receiverAddr_ || *sender == replicaAddrs_[replicaId_]) {
        processQueue_.enqueue(msg);
    } else {
        verifyQueue_.enqueue(msg);
    }
}

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

            if (!sigProvider_.verify(hdr, "replica", reply.replica_id())) {
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

            if (!sigProvider_.verify(hdr, "replica", commitMsg.replica_id())) {
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
            if (!sigProvider_.verify(hdr, "replica", request.replica_id())) {
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
            if (!sigProvider_.verify(hdr, "replica", reply.replica_id())) {
                LOG(INFO) << "Failed to verify replica signature!";
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

            if (!sigProvider_.verify(hdr, "client", requestMsg.client_id())) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        }

#endif

        // Fallback related

        else if (hdr->msgType == FALLBACK_TRIGGER) {
            FallbackTrigger fallbackTriggerMsg;

            if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            if (!fallbackTriggerMsg.has_proof() || verifyFallbackProof(fallbackTriggerMsg.proof()))
                processQueue_.enqueue(msg);

        }

        else if (hdr->msgType == FALLBACK_START) {
            FallbackStart fallbackStartMsg;
            if (!fallbackStartMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse FallbackStart message";
                continue;
            }

            if (!sigProvider_.verify(hdr, "replica", fallbackStartMsg.replica_id())) {
                LOG(INFO) << "Failed to verify replica signature!";
                continue;
            }

            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_PREPREPARE) {
            PBFTPrePrepare PBFTPrePrepareMsg;
            if (!PBFTPrePrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrePrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, "replica", PBFTPrePrepareMsg.primary_id())) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }

            // Verify logs in proposal
            FallbackProposal proposal = PBFTPrePrepareMsg.proposal();
            if (!verifyFallbackProposal(proposal)) {
                LOG(INFO) << "Failed to verify fallback proposal!";
                continue;
            }
            byte tmpDigest[SHA256_DIGEST_LENGTH];
            getProposalDigest(tmpDigest, proposal);
            if (memcmp(tmpDigest, PBFTPrePrepareMsg.proposal_digest().c_str(), SHA256_DIGEST_LENGTH) != 0) {
                LOG(INFO) << "Proposal digest does not match!";
                continue;
            }

            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_PREPARE) {
            PBFTPrepare PBFTPrepareMsg;
            if (!PBFTPrepareMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse PBFTPrepare message";
                continue;
            }
            if (!sigProvider_.verify(hdr, "replica", PBFTPrepareMsg.replica_id())) {
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
            if (!sigProvider_.verify(hdr, "replica", PBFTCommitMsg.replica_id())) {
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
            if (!sigProvider_.verify(hdr, "replica", viewChangeMsg.replica_id())) {
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
            if (!sigProvider_.verify(hdr, "replica", newViewMsg.primary_id())) {
                LOG(INFO) << "Failed to verify primary replica signature for PBFTNewView!";
                continue;
            }
            const auto &viewChanges = newViewMsg.view_changes();
            const auto &sigs = newViewMsg.view_change_sigs();
            bool success = true;
            for (int i = 0; i < viewChanges.size(); i++) {
                if (!sigProvider_.verify(
                        (byte *) viewChanges[i].SerializeAsString().c_str(), viewChanges[i].ByteSizeLong(),
                        (byte *) sigs[i].c_str(), sigs[i].size(), "replica", viewChanges[i].replica_id()
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

            if (fallback_) {
                VLOG(6) << "Queuing request due to fallback";
                fallbackQueuedReqs_.push_back({domHeader.deadline(), clientHeader});
                continue;
            }

            if (swapFreq_ && log_->getNextSeq() == 0)
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

        else if (hdr->msgType == FALLBACK_TRIGGER) {
            FallbackTrigger fallbackTriggerMsg;

            if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
                return;
            }

            processFallbackTrigger(fallbackTriggerMsg, std::span{body + hdr->msgLen, hdr->sigLen});
        }

        if (hdr->msgType == FALLBACK_START) {
            FallbackStart msg;

            if (!msg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
                return;
            }
            processFallbackStart(msg, std::span{body + hdr->msgLen, hdr->sigLen});
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

            processPBFTCommit(msg);
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

void Replica::processClientRequest(const ClientRequest &request)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();

    if (clientId < 0 || clientId > clientAddrs_.size()) {
        LOG(ERROR) << "Invalid client id" << clientId;
        return;
    }

    // 1. Check if client request has been executed in latest checkpoint (i.e. is committed), in which case
    // we should return a CommittedReply, and client only needs f + 1
    if (log_->getStableCheckpoint().clientRecord_.contains(clientId, clientSeq)) {
        LOG(WARNING) << "DUP request c_id=" << clientId << " c_seq=" << clientSeq
                     << " has been committed in previous checkpoint/fallback!, Sending committed reply";

        CommittedReply reply;

        reply.set_replica_id(replicaId_);
        reply.set_client_id(clientId);
        reply.set_client_seq(clientSeq);

        LOG(ERROR) << "TODO Cache for client results not implemented, sending blank result!";

        sendMsgToDst(reply, MessageType::COMMITTED_REPLY, clientAddrs_[clientId]);
        return;
    }

    std::string result;
    uint32_t seq;

    if (!log_->addEntry(clientId, clientSeq, request.req_data(), result)) {
        LOG(ERROR) << "Request dropped since already added (but not committed)!";
        return;
    }

    seq = log_->getNextSeq() - 1;

    VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << seq << " client_id=" << clientId
            << " client_seq=" << clientSeq << " instance=" << instance_
            << " digest=" << digest_to_hex(log_->getDigest());

    Reply reply;
    reply.set_client_id(clientId);
    reply.set_client_seq(clientSeq);
    reply.set_replica_id(replicaId_);
    reply.set_result(result);
    reply.set_seq(seq);
    reply.set_instance(instance_);
    reply.set_digest(log_->getDigest());

    sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);

    // Try and commit every CHECKPOINT_INTERVAL replies
    if (seq % CHECKPOINT_INTERVAL == 0) {
        // Save a digest of the application state and also save a snapshot

        VLOG(2) << "PERF event=checkpoint_start seq=" << seq;
        checkpointCollector_.cacheState(seq, log_->getDigest(seq), log_->getClientRecord(), app_->takeSnapshot());

        // TODO remove execution result from Reply
        broadcastToReplicas(reply, MessageType::REPLY);
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

    if (cert.instance() < instance_) {
        VLOG(2) << "Received stale cert with instance " << cert.instance() << " < " << instance_
                << " for seq=" << r.seq() << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    if (!log_->addCert(cert.seq(), cert)) {
        VLOG(2) << "Failed to add cert for seq=" << r.seq() << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
        return;
    }

    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_);
    reply.set_client_id(r.client_id());
    reply.set_client_seq(r.client_seq());
    reply.set_seq(r.seq());

    VLOG(3) << "Sending cert ack for " << reply.client_id() << ", " << reply.client_seq() << " to "
            << clientAddrs_[reply.client_id()].ip();

    sendMsgToDst(reply, MessageType::CERT_REPLY, clientAddrs_[reply.client_id()]);
}

void Replica::processReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
    uint32_t rSeq = reply.seq();
    if (reply.instance() < instance_) {
        VLOG(4) << "Checkpoint reply seq=" << rSeq << " instance outdated, skipping";
        return;
    }
    VLOG(3) << "Processing reply from replica " << reply.replica_id() << " for seq " << rSeq;

    auto &checkpoint = log_->getStableCheckpoint();

    if (rSeq <= checkpoint.seq) {
        VLOG(4) << "Seq " << rSeq << " is already committed, send back commit and skip it"
                << ". Current checkpoint seq is " << checkpoint.seq;
        return;
    }

    if (checkpointCollector_.addAndCheckReply(reply, sig)) {
        assert(reply.instance() == instance_);

        dombft::proto::Cert cert;
        checkpointCollector_.getCert(instance_, rSeq, cert);
        log_->addCert(rSeq, cert);

        std::string logDigest = log_->getDigest(rSeq);
        std::string appDigest = checkpointCollector_.getCachedState(rSeq).snapshot.digest;

        uint32_t instance = instance_;
        // Broadcast commit Message
        dombft::proto::Commit commit;
        commit.set_replica_id(replicaId_);
        commit.set_seq(rSeq);
        commit.set_instance(instance);
        commit.set_log_digest(logDigest);
        commit.set_app_digest(appDigest);

        checkpointCollector_.getCachedState(rSeq).clientRecord_.toProto(*commit.mutable_client_record());
        broadcastToReplicas(commit, MessageType::COMMIT);
        return;
    }
}

void Replica::processCommit(const dombft::proto::Commit &commit, std::span<byte> sig)
{
    uint32_t seq = commit.seq();
    VLOG(3) << "Processing COMMIT from replica " << commit.replica_id() << " for seq " << seq;
    if (commit.instance() < instance_) {
        VLOG(4) << "Checkpoint commit instance outdated, skipping";
        return;
    }
    if (seq <= log_->getStableCheckpoint().seq) {
        VLOG(4) << "Seq " << seq << " is already committed, skipping";
        return;
    }

    // add current commit msg to collector
    // use the majority agreed commit message if exists
    if (checkpointCollector_.addAndCheckCommit(commit, sig)) {
        // TODO we can update our instance in case commit.instance() > instance_
        assert(instance_ == commit.instance());
        dombft::proto::Commit commitToUse;
        checkpointCollector_.getCommitToUse(commit.instance(), seq, commitToUse);

        if (checkpointDropFreq_ && seq / CHECKPOINT_INTERVAL % checkpointDropFreq_ == 0) {
            LOG(INFO) << "Dropping checkpoint seq=" << seq;
            return;
        }
        LOG(INFO) << "Try to commit seq=" << seq;

        uint32_t seq = commitToUse.seq();

        if (seq >= log_->getNextSeq() || log_->getDigest(seq) != commitToUse.log_digest()) {
            LOG(INFO) << "My log digest does not match the commit message digest requesting snapshot...";
            // TODO choose a random replica from those that have this
            sendSnapshotRequest(commitToUse.replica_id(), commitToUse.seq());

        } else {
            ::LogCheckpoint checkpoint;
            checkpointCollector_.getCheckpoint(commit.instance(), seq, checkpoint);
            log_->setStableCheckpoint(checkpoint);
            // TODO move this and use delta information...
            appSnapshotStore_.addSnapshot(std::string(checkpointCollector_.getCachedState(seq).snapshot.snapshot), seq);
        }

        // if there is overlapping and later checkpoint commits first, skip earlier ones
        checkpointCollector_.cleanStaleCollectors(seq, instance_);

        // Unregister fallbackStart timer set by request timeout in slow path
        //  as checkpoint confirms progress
        fallbackTriggerTime_ = 0;
    }
}

void Replica::processSnapshotRequest(const SnapshotRequest &request)
{
    uint32_t reqSeq = request.seq();
    uint32_t instance = request.instance();
    VLOG(3) << "Processing SNAPSHOT_REQUEST from replica " << request.replica_id() << " for seq " << reqSeq;
    if (reqSeq > log_->getStableCheckpoint().seq) {
        VLOG(4) << "Requested req " << reqSeq << " is ahead of current checkpoint, cannot provide snapshot";
        return;
    }
    // In fact, returned state snapshot is always the latest snapshot (with the checkpoint.seq)
    // the LOG is just to make it clear.

    // TODO, use request's last checkpoint sequence to return a delta instead..

    SnapshotReply snapshotReply;
    snapshotReply.set_instance(instance_);
    snapshotReply.set_seq(log_->getStableCheckpoint().seq);
    snapshotReply.set_replica_id(replicaId_);
    snapshotReply.set_pbft_view(pbftView_);
    snapshotReply.set_snapshot(appSnapshotStore_.getSnapshot());

    sendMsgToDst(snapshotReply, MessageType::SNAPSHOT_REPLY, replicaAddrs_[request.replica_id()]);
}

void Replica::processSnapshotReply(const dombft::proto::SnapshotReply &snapshotReply)
{
    if (snapshotReply.instance() < instance_) {
        VLOG(4) << "Snapshot reply instance outdated, skipping";
        return;
    }
    if (snapshotReply.seq() <= log_->getStableCheckpoint().seq) {
        VLOG(4) << "Seq " << snapshotReply.seq() << " is already committed, skipping";
        return;
    }
    LOG(INFO) << "Processing SNAPSHOT_REPLY from replica " << snapshotReply.replica_id() << " for seq "
              << snapshotReply.seq();

    if (fallback_) {
        // Finish applying LogSuffix computed from fallback proposal and return to normal processing
        LogSuffix &logSuffix = getFallbackLogSuffix();
        // TODO make sure this isn't outdated...

        applySuffixWithSnapshot(logSuffix, log_, snapshotReply.snapshot());
        appSnapshotStore_.addSnapshot(std::string(snapshotReply.snapshot()), snapshotReply.seq());

    } else {
        // Apply snapshot from checkpoint and reorder my log
        // TODO make sure this isn't outdated...

        ::LogCheckpoint checkpoint;
        checkpointCollector_.getCheckpoint(instance_, snapshotReply.seq(), checkpoint);

        if (!log_->applySnapshotModifyLog(snapshotReply.seq(), checkpoint, snapshotReply.snapshot())) {
            LOG(ERROR) << "Failed to apply snapshot because it did not match digest!";
            // TODO handle this better
            throw std::runtime_error("Snapshot digest mismatch");
        }
        appSnapshotStore_.addSnapshot(std::string(snapshotReply.snapshot()), snapshotReply.seq());

        // Resend replies after modifying log
        for (int seq = log_->getStableCheckpoint().seq + 1; seq < log_->getNextSeq(); seq++) {
            auto &entry = log_->getEntry(seq);

            Reply reply;

            reply.set_client_id(entry.client_id);
            reply.set_client_seq(entry.client_seq);
            reply.set_replica_id(replicaId_);
            reply.set_result(entry.result);
            reply.set_seq(seq);
            reply.set_instance(instance_);
            reply.set_digest(entry.digest);

            VLOG(1) << "PERF event=update_digest seq=" << seq << " digest=" << digest_to_hex(entry.digest).substr(56)
                    << " c_id=" << entry.client_id << " c_seq=" << entry.client_seq;

            sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[entry.client_id]);
        }
    }
}

void Replica::processFallbackTrigger(const dombft::proto::FallbackTrigger &msg, std::span<byte> sig)
{
    // Ignore repeated fallback triggers
    if (fallback_) {
        LOG(WARNING) << "Received fallback trigger during a fallback from client " << msg.client_id()
                     << " for cseq=" << msg.client_seq();
        return;
    }

    if (!msg.has_proof() && fallbackTriggerTime_ != 0) {
        LOG(WARNING) << "Received redundant fallback trigger due to client side timeout from client_id="
                     << msg.client_id() << " for cseq=" << msg.client_seq();
        return;
    }

    LOG(INFO) << "Received fallback trigger from client_id=" << msg.client_id() << " for cseq=" << msg.client_seq();

    if (msg.has_proof()) {
        // Proof is verified by verify thread
        if (msg.proof().instance() < instance_) {
            LOG(INFO) << "Received fallback trigger proof for previous instance " << msg.proof().instance() << " < "
                      << instance_;
            return;
        }

        LOG(INFO) << "Fallback trigger has a proof, starting fallback!";

        // TODO we need to broadcast this with the original signature
        broadcastToReplicas(msg, FALLBACK_TRIGGER);
        startFallback();
    } else {
        fallbackTriggerTime_ = GetMicrosecondTimestamp();
    }
}

void Replica::processFallbackStart(const FallbackStart &msg, std::span<byte> sig)
{
    if ((msg.pbft_view() % replicaAddrs_.size()) != replicaId_) {
        LOG(INFO) << "Received FALLBACK_START for instance " << msg.instance() << " pbft_view " << msg.pbft_view()
                  << " where I am not proposer";
        return;
    }

    uint32_t repId = msg.replica_id();
    uint32_t repInstance = msg.instance();
    if (msg.instance() < instance_) {
        LOG(INFO) << "Received FALLBACK_START for instance " << msg.instance() << " from replica " << repId
                  << " while own is " << instance_;
        return;
    }

    // A corner case where (older instance + pbft_view) targets the same primary and overwrite the newer ones
    if (fallbackHistorys_.count(repId) && fallbackHistorys_[repId].instance() >= repInstance) {
        LOG(INFO) << "Received FALLBACK_START for instance " << repInstance << " pbft_view " << msg.pbft_view()
                  << " from replica " << repId << " which is outdated";
        return;
    }
    fallbackHistorys_[repId] = msg;
    fallbackHistorySigs_[repId] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received fallbackStart message from replica " << repId;

    if (!isPrimary()) {
        return;
    }

    // First check if we have 2f + 1 fallback start messages for the same instance
    auto numStartMsgs = std::count_if(fallbackHistorys_.begin(), fallbackHistorys_.end(), [&](auto &startMsg) {
        return startMsg.second.instance() == repInstance;
    });

    if (numStartMsgs == 2 * f_ + 1) {
        doPrePreparePhase(repInstance);
    }
}

void Replica::checkTimeouts()
{
    uint64_t now = GetMicrosecondTimestamp();

    if (fallbackTriggerTime_ != 0 && now - fallbackTriggerTime_ > fallbackTimeout_) {
        fallbackTriggerTime_ = 0;
        LOG(WARNING) << "fallbackStartTimer for instance=" << instance_ << " timed out! Starting fallback";
        this->startFallback();   // Note this changes fallbackStartTime, so we need to get the time again
    };

    now = GetMicrosecondTimestamp();

    if (fallbackStartTime_ != 0 && now - fallbackStartTime_ > viewChangeTimeout_) {
        fallbackStartTime_ = now;
        LOG(WARNING) << "Fallback for instance=" << instance_ << " pbft_view=" << pbftView_ << " failed (timed out)!";
        pbftViewChanges_.clear();
        pbftViewChangeSigs_.clear();
        this->startViewChange();
    };
}

// ============== Sending Helpers ==============

void Replica::sendSnapshotRequest(uint32_t replicaId, uint32_t targetSeq)
{
    SnapshotRequest snapshotRequest;
    snapshotRequest.set_replica_id(replicaId_);
    snapshotRequest.set_seq(targetSeq);
    snapshotRequest.set_last_checkpoint_seq(log_->getStableCheckpoint().seq);
    snapshotRequest.set_instance(instance_);
    sendMsgToDst(snapshotRequest, MessageType::SNAPSHOT_REQUEST, replicaAddrs_[replicaId]);
    LOG(INFO) << "Requesting state snapshot for seq " << targetSeq << " from replica " << replicaId;
}

template <typename T> void Replica::sendMsgToDst(const T &msg, MessageType type, const Address &dst)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        endpoint_->SendPreparedMsgTo(dst, hdr);
    });
}

template <typename T> void Replica::broadcastToReplicas(const T &msg, MessageType type)
{
    sendThreadpool_.enqueueTask([=, this](byte *buffer) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buffer);
        sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
        for (const Address &addr : replicaAddrs_) {
            endpoint_->SendPreparedMsgTo(addr, hdr);
        }
    });
}

// ============== Verify Helpers ==============

bool Replica::verifyCert(const Cert &cert)
{
    if (cert.replies().size() < 2 * f_ + 1) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_;
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

        ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
                        reply.client_seq(), reply.digest(),   reply.result()};
        matchingReplies[key].insert(replicaId);

        std::string serializedReply = reply.SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(), "replica",
                reply.replica_id()
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

bool Replica::verifyFallbackProof(const Cert &proof)
{
    if (proof.replies().size() < f_ + 1) {
        LOG(INFO) << "Received fallback proof of size " << proof.replies().size()
                  << ", which is smaller than f + 1, f=" << f_;
        return false;
    }

    if (proof.replies().size() != proof.signatures().size()) {
        LOG(WARNING) << "Proof replies size " << proof.replies().size() << " is not equal to " << "cert signatures size"
                     << proof.signatures().size();
        return false;
    }

    uint32_t instance = proof.instance();

    // check if the replies are matching and no duplicate replies from same replica
    std::map<ReplyKey, std::unordered_set<uint32_t>> matchingReplies;
    // Verify each signature in the proof
    for (int i = 0; i < proof.replies_size(); i++) {
        const Reply &reply = proof.replies()[i];
        const std::string &sig = proof.signatures()[i];
        uint32_t replicaId = reply.replica_id();

        if (instance != reply.instance()) {
            LOG(INFO) << "Proof has replies from different instances!";
            return false;
        }

        ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
                        reply.client_seq(), reply.digest(),   reply.result()};

        matchingReplies[key].insert(replicaId);
        std::string serializedReply = proof.replies(i).SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(), "replica",
                reply.replica_id()
            )) {
            LOG(INFO) << "Proof failed to verify!";
            return false;
        }
    }

    if (matchingReplies.size() == 1) {
        LOG(WARNING) << "Proof does not have non-matching replies!";
        return false;
    }
    uint32_t sum = 0;
    for (auto &[_, s] : matchingReplies) {
        sum += s.size();
    }

    if (sum < proof.replies().size()) {
        LOG(WARNING) << "Proof has replies from the same replica!";
        return false;
    }
    return true;
}

bool Replica::verifyFallbackProposal(const FallbackProposal &proposal)
{
    std::vector<std::tuple<byte *, uint32_t>> logSigs;
    for (auto &sig : proposal.signatures()) {
        logSigs.emplace_back((byte *) sig.data(), sig.length());
    }
    uint32_t ind = 0;
    for (auto &log : proposal.logs()) {
        std::string logStr = log.SerializeAsString();
        byte *logBuffer = (byte *) logStr.data();
        byte *logSig = std::get<0>(logSigs[ind]);
        uint32_t logSigLen = std::get<1>(logSigs[ind]);
        ind++;
        if (!sigProvider_.verify(logBuffer, logStr.length(), logSig, logSigLen, "replica", log.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature in proposal!";
            return false;
        }
    }
    return true;
}

bool Replica::verifyViewChange(const PBFTViewChange &viewChangeMsg)
{
    // verify prepares
    const auto &prepares = viewChangeMsg.prepares();
    const auto &sigs = viewChangeMsg.prepare_sigs();

    if (prepares.empty() && viewChangeMsg.instance() == UINT32_MAX) {
        LOG(INFO) << "View Change with no previous agreed prepares";
        return true;
    }
    std::unordered_set<uint32_t> insts;
    std::unordered_set<std::string> proposal_digests;
    for (int i = 0; i < prepares.size(); i++) {
        if (!sigProvider_.verify(
                (byte *) prepares[i].SerializeAsString().c_str(), prepares[i].ByteSizeLong(), (byte *) sigs[i].c_str(),
                sigs[i].size(), "replica", prepares[i].replica_id()
            )) {
            LOG(INFO) << "Failed to verify replica signature in view change!";
            return false;
        }
        insts.emplace(prepares[i].instance());
        proposal_digests.emplace(prepares[i].proposal_digest());
    }
    if (insts.size() != 1 || proposal_digests.size() != 1) {
        LOG(INFO) << "View Change with inconsistent prepares and proposals from replica " << viewChangeMsg.replica_id()
                  << " instance=" << viewChangeMsg.instance() << " pbf_view=" << viewChangeMsg.pbft_view();
        return false;
    }
    if (insts.find(viewChangeMsg.instance()) == insts.end()) {
        LOG(INFO) << "View Change with different instance";
        return false;
    }
    const FallbackProposal &proposal = viewChangeMsg.proposal();
    if (!verifyFallbackProposal(proposal)) {
        LOG(INFO) << "Failed to verify fallback proposal!";
        return false;
    }
    return true;
}

// ============== Fallback ==============

void Replica::startFallback()
{
    assert(!fallback_);
    fallback_ = true;
    LOG(INFO) << "Starting fallback on instance " << instance_;

    // Start fallback timer to change primary if timeout
    fallbackStartTime_ = GetMicrosecondTimestamp();

    VLOG(1) << "PERF event=fallback_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " instance=" << instance_ << " pbft_view=" << pbftView_;

    // Extract log into start fallback message
    FallbackStart fallbackStartMsg;
    fallbackStartMsg.set_instance(instance_);
    fallbackStartMsg.set_replica_id(replicaId_);
    fallbackStartMsg.set_pbft_view(pbftView_);
    log_->toProto(fallbackStartMsg);

    uint32_t primaryId = getPrimary();
    LOG(INFO) << "Sending FALLBACK_START to PBFT primary replica " << primaryId;
    sendMsgToDst(fallbackStartMsg, FALLBACK_START, replicaAddrs_[primaryId]);
    LOG(INFO) << "DUMP start fallback instance=" << instance_ << " " << *log_;
}

void Replica::replyFromLogEntry(Reply &reply, uint32_t seq)
{
    const ::LogEntry &entry = log_->getEntry(seq);   // TODO better namespace

    reply.set_client_id(entry.client_id);
    reply.set_client_seq(entry.client_seq);
    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_);
    reply.set_result(entry.result);
    reply.set_seq(entry.seq);
    reply.set_digest(entry.digest);
}

void Replica::sendFallbackSummaryToClients()
{
    FallbackSummary summary;
    std::set<int> clients;

    summary.set_instance(instance_);
    summary.set_replica_id(replicaId_);
    summary.set_pbft_view(pbftView_);

    uint32_t seq = log_->getStableCheckpoint().seq;
    for (; seq < log_->getNextSeq(); seq++) {
        const ::LogEntry &entry = log_->getEntry(seq);   // TODO better namespace
        CommittedReply reply;

        clients.insert(entry.client_id);

        reply.set_replica_id(replicaId_);
        reply.set_client_id(entry.client_id);
        reply.set_client_seq(entry.client_seq);
        reply.set_seq(entry.seq);
        reply.set_result(entry.result);

        (*summary.add_replies()) = reply;
    }

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(summary, MessageType::FALLBACK_SUMMARY);
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

    // TODO make this only send to clients that need it
    LOG(INFO) << "Sending fallback summary for instance=" << instance_;
    for (auto &addr : clientAddrs_) {
        endpoint_->SendPreparedMsgTo(addr);
    }
}

void Replica::finishFallback()
{
    VLOG(1) << "PERF event=fallback_end replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " instance=" << instance_ << " pbft_view=" << pbftView_;

    LOG(INFO) << "DUMP finish fallback instance=" << instance_ << " " << *log_;

    instance_++;
    LOG(INFO) << "Instance updated to " << instance_ << " and pbft_view to " << pbftView_;

    if (viewChange_) {
        viewChangeInst_ += viewChangeFreq_;
        viewChange_ = false;
        viewChangeCounter_ += 1;
    }

    fallback_ = false;
    fallbackProposal_.reset();
    fallbackPrepares_.clear();
    fallbackPBFTCommits_.clear();
    fallbackProposalLogSuffix_.reset();

    // TODO: since the fallback is PBFT, we can simply set the checkpoint here already using the PBFT messages as proofs
    // For the sake of implementation simplicity, we just trigger the usual checkpointing process
    // However, client requests are still safe, as even if the next fallback is triggered before this checkpoint
    // finishes, the client requests will have f + 1 replicas that executed it in their logs

    uint32_t seq = log_->getNextSeq() - 1;

    checkpointCollector_.cacheState(seq, log_->getDigest(seq), log_->getClientRecord(), app_->takeSnapshot());

    Reply reply;
    replyFromLogEntry(reply, seq);
    broadcastToReplicas(reply, MessageType::REPLY);

    for (auto &[_, req] : fallbackQueuedReqs_) {
        VLOG(5) << "Processing queued request client_id=" << req.client_id() << " client_seq=" << req.client_seq();
        processClientRequest(req);
    }
    fallbackQueuedReqs_.clear();
}

void Replica::tryFinishFallback()
{
    assert(fallbackProposal_.has_value());
    if (fallbackProposal_.value().instance() == instance_ - 1) {
        // This happens if the fallback instance is already committed on the current replica, but other replicas
        // initiated a view change.
        assert(viewChange_);
        LOG(INFO) << "Fallback on instance " << instance_ - 1 << " already committed on current replica, skipping";
        return;
    }

    FallbackProposal &proposal = fallbackProposal_.value();
    instance_ = proposal.instance();
    // Reset timer
    fallbackStartTime_ = 0;

    LogSuffix &logSuffix = getFallbackLogSuffix();

    // Check if own checkpoint seq is behind suffix checkpoint seq
    const dombft::proto::LogCheckpoint *checkpoint = logSuffix.checkpoint;

    ::LogCheckpoint &myCheckpoint = log_->getStableCheckpoint();   // bad namespace
    if (checkpoint->seq() > myCheckpoint.seq) {
        // If our log is consistent, we can just use the checkpoint, otherwise
        // we need to request a snapshot from the replica that has the checkpoint

        if (checkpoint->log_digest() == log_->getDigest(checkpoint->seq())) {
            LOG(INFO) << "Fallback checkpoint seq=" << checkpoint->seq()
                      << " is ahead of myCheckpoint.seq=" << myCheckpoint.seq
                      << ", applying checkpoint before fallback";
            log_->setStableCheckpoint(*checkpoint);

            applySuffixWithoutSnapshot(logSuffix, log_);
            finishFallback();
        } else {
            LOG(INFO) << "Fallback checkpoint seq=" << checkpoint->seq()
                      << " is inconsistent with my log, snapshot will be fetched from replicaId=";
            sendSnapshotRequest(logSuffix.checkpointReplica, checkpoint->seq());
        }

    } else {
        applySuffixWithoutSnapshot(logSuffix, log_);
        finishFallback();
    }
}

LogSuffix &Replica::getFallbackLogSuffix()
{
    // This is just to cache the processing of the fallbackProposal
    // TODO make sure this works during view change as well.
    if (!fallbackProposalLogSuffix_.has_value() || fallbackProposalLogSuffix_.value().instance != instance_) {
        fallbackProposalLogSuffix_ = LogSuffix();
        fallbackProposalLogSuffix_->replicaId = replicaId_;
        fallbackProposalLogSuffix_->instance = instance_;
        getLogSuffixFromProposal(fallbackProposal_.value(), fallbackProposalLogSuffix_.value());
    }
    return fallbackProposalLogSuffix_.value();
}

void Replica::doPrePreparePhase(uint32_t instance)
{
    if (!isPrimary()) {
        LOG(ERROR) << "Attempted to doPrePrepare from non-primary replica!";
        return;
    }
    LOG(INFO) << "PrePrepare for pbft_view=" << pbftView_ << " in primary replicaId=" << replicaId_;

    PBFTPrePrepare prePrepare;
    prePrepare.set_primary_id(replicaId_);
    prePrepare.set_instance(instance);
    prePrepare.set_pbft_view(pbftView_);
    // Piggyback the fallback proposal
    FallbackProposal *proposal = prePrepare.mutable_proposal();
    proposal->set_replica_id(replicaId_);
    proposal->set_instance(instance);
    for (auto &startMsg : fallbackHistorys_) {
        if (startMsg.second.instance() != instance)
            continue;

        *(proposal->add_logs()) = startMsg.second;
        *(proposal->add_signatures()) = fallbackHistorySigs_[startMsg.first];
    }
    getProposalDigest(proposalDigest_, *proposal);
    prePrepare.set_proposal_digest(proposalDigest_, SHA256_DIGEST_LENGTH);
    broadcastToReplicas(prePrepare, PBFT_PREPREPARE);
}

void Replica::doPreparePhase()
{
    LOG(INFO) << "Prepare for instance=" << fallbackProposal_->instance() << " replicaId=" << replicaId_;
    PBFTPrepare prepare;
    prepare.set_replica_id(replicaId_);
    prepare.set_instance(fallbackProposal_->instance());
    prepare.set_pbft_view(pbftView_);
    prepare.set_proposal_digest(proposalDigest_, SHA256_DIGEST_LENGTH);
    broadcastToReplicas(prepare, PBFT_PREPARE);
}

void Replica::doCommitPhase()
{
    uint32_t proposalInst = fallbackProposal_.value().instance();
    LOG(INFO) << "PBFTCommit for instance=" << proposalInst << " replicaId=" << replicaId_;
    PBFTCommit cmt;
    cmt.set_replica_id(replicaId_);
    cmt.set_instance(proposalInst);
    cmt.set_pbft_view(pbftView_);
    cmt.set_proposal_digest(proposalDigest_, SHA256_DIGEST_LENGTH);
    if (viewChangeByCommit()) {
        if (commitLocalInViewChange_)
            sendMsgToDst(cmt, PBFT_COMMIT, replicaAddrs_[replicaId_]);
        holdPrepareOrCommit_ = !holdPrepareOrCommit_;
        return;
    }
    broadcastToReplicas(cmt, PBFT_COMMIT);
}

void Replica::processPrePrepare(const PBFTPrePrepare &msg)
{
    if (msg.instance() < instance_) {
        LOG(INFO) << "Received old fallback preprepare from instance=" << msg.instance() << " own instance is "
                  << instance_;
        return;
    }
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received preprepare from replicaId=" << msg.primary_id() << " for instance=" << msg.instance()
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (getPrimary() != msg.primary_id()) {
        LOG(INFO) << "Received fallback preprepare from non-primary replica " << msg.primary_id()
                  << ". Current selected primary is " << getPrimary();
        return;
    }

    LOG(INFO) << "PrePrepare RECEIVED for instance=" << msg.instance() << " from replicaId=" << msg.primary_id();
    // accepts the proposal as long as it's from the primary
    fallbackProposal_ = msg.proposal();
    memcpy(proposalDigest_, msg.proposal_digest().c_str(), SHA256_DIGEST_LENGTH);

    if (viewChangeByPrepare()) {
        holdPrepareOrCommit_ = !holdPrepareOrCommit_;
        LOG(INFO) << "Prepare message held to cause timeout in prepare phase for view change";
        return;
    }
    doPreparePhase();
}

void Replica::processPrepare(const PBFTPrepare &msg, std::span<byte> sig)
{
    uint32_t inInst = msg.instance();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received prepare from replicaId=" << msg.replica_id() << " for instance=" << inInst
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inInst < instance_ && viewPrepared_) {
        LOG(INFO) << "Received old fallback prepare from instance=" << inInst << " own instance is " << instance_;
        return;
    }

    if (fallbackPrepares_.count(msg.replica_id()) && fallbackPrepares_[msg.replica_id()].instance() > inInst &&
        fallbackPrepares_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old prepare received from replicaId=" << msg.replica_id() << " for instance=" << inInst;
        return;
    }
    fallbackPrepares_[msg.replica_id()] = msg;
    fallbackPrepareSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());
    LOG(INFO) << "Prepare RECEIVED for instance=" << inInst << " from replicaId=" << msg.replica_id();
    // skip if already prepared for it
    // note: if viewPrepared_==false, then viewChange_==true
    if (viewPrepared_ && preparedInstance_ == inInst) {
        LOG(INFO) << "Already prepared for instance=" << inInst << " pbft_view=" << pbftView_;
        return;
    }
    if (!fallbackProposal_.has_value() || fallbackProposal_.value().instance() < inInst) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process prepare";
        return;
    }
    // Make sure the Prepare msgs are for the corresponding PrePrepare msg
    auto numMsgs = std::count_if(fallbackPrepares_.begin(), fallbackPrepares_.end(), [this](auto &curMsg) {
        return curMsg.second.instance() == fallbackProposal_.value().instance() &&
               memcmp(curMsg.second.proposal_digest().c_str(), proposalDigest_, SHA256_DIGEST_LENGTH) == 0 &&
               curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < 2 * f_ + 1) {
        LOG(INFO) << "Prepare received from " << numMsgs << " replicas, waiting for 2f + 1 to proceed";
        return;
    }
    // Store PBFT states for potential view change
    preparedInstance_ = fallbackProposal_.value().instance();
    viewPrepared_ = true;
    pbftState_.proposal = fallbackProposal_.value();
    memcpy(pbftState_.proposal_digest, proposalDigest_, SHA256_DIGEST_LENGTH);
    pbftState_.prepares.clear();
    for (const auto &[repId, prepare] : fallbackPrepares_) {
        if (prepare.instance() == preparedInstance_) {
            pbftState_.prepares[repId] = prepare;
            pbftState_.prepareSigs[repId] = fallbackPrepareSigs_[repId];
        }
    }
    LOG(INFO) << "Prepare received from 2f + 1 replicas, agreement reached for instance=" << preparedInstance_
              << " pbft_view=" << pbftView_;

    if (viewChangeByCommit()) {
        if (commitLocalInViewChange_) {
            LOG(INFO) << "Commit message only send to itself to commit locally to advance to next instance";
            doCommitPhase();
        } else {
            LOG(INFO) << "Commit message held to cause timeout in commit phase for view change";
        }
        return;
    }
    doCommitPhase();
}

void Replica::processPBFTCommit(const PBFTCommit &msg)
{
    uint32_t inInst = msg.instance();
    if (msg.pbft_view() != pbftView_) {
        LOG(INFO) << "Received commit from replicaId=" << msg.replica_id() << " for instance=" << inInst
                  << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if (inInst < instance_ && !viewChange_) {
        LOG(INFO) << "Received old fallback commit from instance=" << inInst << " own instance is " << instance_;
        return;
    }
    if (fallbackPBFTCommits_.count(msg.replica_id()) && fallbackPBFTCommits_[msg.replica_id()].instance() > inInst &&
        fallbackPBFTCommits_[msg.replica_id()].pbft_view() == msg.pbft_view()) {
        LOG(INFO) << "Old commit received from replicaId=" << msg.replica_id() << " for instance=" << inInst;
        return;
    }
    fallbackPBFTCommits_[msg.replica_id()] = msg;
    LOG(INFO) << "PBFTCommit RECEIVED for instance=" << inInst << " from replicaId=" << msg.replica_id();

    if (!fallbackProposal_.has_value() || fallbackProposal_.value().instance() < inInst) {
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process commit";
        return;
    }

    if (preparedInstance_ == UINT32_MAX || preparedInstance_ != inInst || !viewPrepared_) {
        LOG(INFO) << "Not prepared for it, skipping commit!";
        // TODO get the proposal from another replica...
        return;
    }
    auto numMsgs = std::count_if(fallbackPBFTCommits_.begin(), fallbackPBFTCommits_.end(), [this](auto &curMsg) {
        return curMsg.second.instance() == preparedInstance_ &&
               memcmp(curMsg.second.proposal_digest().c_str(), proposalDigest_, SHA256_DIGEST_LENGTH) == 0 &&
               curMsg.second.pbft_view() == pbftView_;
    });
    if (numMsgs < 2 * f_ + 1) {
        return;
    }
    LOG(INFO) << "Commit received from 2f + 1 replicas, Committed!";
    tryFinishFallback();
}

void Replica::startViewChange()
{
    pbftView_++;
    fallback_ = true;
    viewChange_ = true;
    viewPrepared_ = false;
    VLOG(1) << "PERF event=viewchange_start replica_id=" << replicaId_ << " seq=" << log_->getNextSeq()
            << " instance=" << instance_ << " pbft_view=" << pbftView_;
    LOG(INFO) << "Starting ViewChange on instance " << instance_ << " pbft_view " << pbftView_;

    PBFTViewChange viewChange;
    viewChange.set_replica_id(replicaId_);
    viewChange.set_instance(preparedInstance_);
    viewChange.set_pbft_view(pbftView_);

    // Add the latest quorum of prepares and sigs
    if (preparedInstance_ != UINT32_MAX) {
        for (const auto &[repId, prepare] : pbftState_.prepares) {
            *(viewChange.add_prepares()) = prepare;
            *(viewChange.add_prepare_sigs()) = pbftState_.prepareSigs[repId];
        }
        viewChange.set_proposal_digest(pbftState_.proposal_digest, SHA256_DIGEST_LENGTH);
        viewChange.mutable_proposal()->CopyFrom(pbftState_.proposal);
    }

    broadcastToReplicas(viewChange, PBFT_VIEWCHANGE);
}

void Replica::processPBFTViewChange(const PBFTViewChange &msg, std::span<byte> sig)
{
    // No instance checking as view change is not about instance
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

    if (numMsgs != 2 * f_ + 1) {
        return;
    }
    LOG(INFO) << "ViewChange for view " << inViewNum << " received from 2f + 1 replicas!";

    // non-primary replicas collect view change msgs for
    // 1. delaying timer setting for better liveness (avoid frequent view changes)
    // 2. check if the majority has a larger view# and start view change if so (avoid starting view change too late)

    fallbackStartTime_ = GetMicrosecondTimestamp();   // reset view change timeout (1) above

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

    // primary search for the latest prepared instance
    uint32_t maxInstance = UINT32_MAX;
    // vc.instance can be UINT32_MAX if no prepares, which is fine
    for (const auto &[_, vc] : pbftViewChanges_) {
        if (vc.pbft_view() != inViewNum) {
            continue;
        }
        if (maxInstance == UINT32_MAX && vc.instance() < maxInstance) {
            maxInstance = vc.instance();
        } else if (maxInstance != UINT32_MAX && vc.instance() > maxInstance) {
            maxInstance = vc.instance();
        }
    }
    LOG(INFO) << "Found the latest prepared instance=" << maxInstance << " for pbft_view=" << inViewNum;

    PBFTNewView newView;
    newView.set_primary_id(replicaId_);
    newView.set_pbft_view(inViewNum);
    newView.set_instance(maxInstance);

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
        if (vc.instance() > maxVC.instance()) {
            maxVC = vc;
        }
    }
    if (maxVC.pbft_view() != msg.pbft_view() || maxVC.instance() != msg.instance()) {
        LOG(INFO) << "Replica obtains a different choice of instance=" << maxVC.instance() << "," << msg.instance()
                  << " and pbft_view=" << maxVC.pbft_view() << ", " << msg.pbft_view();
    }
    if (views[maxVC.pbft_view()] < 2 * f_ + 1) {
        LOG(INFO) << "The view number " << maxVC.pbft_view() << " does not have a 2f + 1 quorum";
        return;
    }
    LOG(INFO) << "Received NewView for pbft_view=" << msg.pbft_view() << " with prepared instance=" << msg.instance();
    if (pbftView_ == msg.pbft_view() && viewPrepared_) {
        LOG(INFO) << "Already in the same view and prepared for it, skip the new view message";
        return;
    }
    pbftView_ = msg.pbft_view();
    // in case it is not in view change already. Not quite sure this is correct way tho
    if (!viewChange_) {
        viewChange_ = true;
        fallback_ = true;
        viewPrepared_ = false;
        fallbackProposal_.reset();
        fallbackPrepares_.clear();
        fallbackPBFTCommits_.clear();
    }

    // TODO(Hao): test this corner case later
    if (msg.instance() == UINT32_MAX) {
        LOG(INFO) << "No previously prepared instance in new view, go back to normal state. EXIT FOR NOW";
        assert(msg.instance() != UINT32_MAX);
        return;
    }
    fallbackProposal_ = maxVC.proposal();
    memcpy(proposalDigest_, maxVC.proposal_digest().c_str(), SHA256_DIGEST_LENGTH);
    // set view change param to true to bypass instance check.
    doPreparePhase();
}

void Replica::getProposalDigest(byte *digest, const FallbackProposal &proposal)
{
    // use signatures as digest, since signatures are can function as the the digest of each proposal
    std::string digestStr;
    for (const auto &sig : proposal.signatures()) {
        digestStr += sig;
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, digestStr.c_str(), digestStr.size());
    SHA256_Final(digest, &ctx);
}

}   // namespace dombft
