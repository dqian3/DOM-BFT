#include "replica.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/common.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <openssl/pem.h>
#include <sstream>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t swapFreq, uint32_t viewChangeFreq, bool commitLocalInViewChange)
    : replicaId_(replicaId)
    , f_(config.replicaIps.size() / 3)
    , numVerifyThreads_(config.replicaNumVerifyThreads)
    , sigProvider_()
    , sendThreadpool_(config.replicaNumSendThreads)
    , instance_(0)
    , checkpointCollectors_(replicaId_, f_)
    , swapFreq_(swapFreq)
    , viewChangeFreq_(viewChangeFreq)
    , viewChangeInst_(viewChangeFreq_)
    , commitLocalInViewChange_(commitLocalInViewChange)
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
        log_ = std::make_shared<Log>(std::make_shared<Counter>());
    } else {
        LOG(ERROR) << "Unrecognized App Type";
        exit(1);
    }
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

    fallbackStartTimer_ = std::make_unique<Timer>(
        [this](void *ctx, void *endpoint) {
            endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
            LOG(WARNING) << "fallbackStartTimer for instance=" << instance_ << " timed out! Starting fallback";
            this->startFallback();
        },
        config.replicaFallbackStartTimeout, this
    );

    fallbackTimer_ = std::make_unique<Timer>(
        [this](void *ctx, void *endpoint) {
            LOG(WARNING) << "Fallback for instance=" << instance_ << " pbft_view="<<pbftView_<<" failed (timed out)!";
            endpoint_->UnRegisterTimer(fallbackTimer_.get());
            if(endpoint_->isTimerRegistered(fallbackStartTimer_.get())){
                endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
            }
            pbftViewChanges_.clear();
            pbftViewChangeSigs_.clear();
            this->startViewChange();
        },
        config.replicaFallbackTimeout, this
    );

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

        else if (hdr->msgType == FALLBACK_TRIGGER) {
            FallbackTrigger fallbackTriggerMsg;

            if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse COMMIT message";
                continue;
            }

            if (!sigProvider_.verify(hdr, "client", fallbackTriggerMsg.client_id())) {
                LOG(INFO) << "Failed to verify client signature from " << fallbackTriggerMsg.client_id();
                continue;
            }

            if(!fallbackTriggerMsg.has_proof() || verifyFallbackProof(fallbackTriggerMsg.proof()))
                processQueue_.enqueue(msg);

        }

        // TODO(Hao): use polymorphism to avoid parsing twice?
        else if (hdr->msgType == FALLBACK_START) {
            FallbackStart fallbackStartMsg;
            if(!fallbackStartMsg.ParseFromArray(body, hdr->msgLen)){
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
            if(!PBFTPrePrepareMsg.ParseFromArray(body, hdr->msgLen)){
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
            if(!PBFTPrepareMsg.ParseFromArray(body, hdr->msgLen)){
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
            if(!PBFTCommitMsg.ParseFromArray(body, hdr->msgLen)){
                LOG(ERROR) << "Unable to parse PBFTCommit message";
                continue;
            }
            if (!sigProvider_.verify(hdr, "replica", PBFTCommitMsg.replica_id())) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            processQueue_.enqueue(msg);
        } else if (hdr->msgType == PBFT_VIEWCHANGE){
            PBFTViewChange viewChangeMsg;
            if(!viewChangeMsg.ParseFromArray(body, hdr->msgLen)){
                LOG(ERROR) << "Unable to parse PBFTViewChange message";
                continue;
            }
            if (!sigProvider_.verify(hdr, "replica", viewChangeMsg.replica_id())) {
                LOG(INFO) << "Failed to verify primary replica signature!";
                continue;
            }
            if(!verifyViewChange(viewChangeMsg)) continue;
            processQueue_.enqueue(msg);

        }else if (hdr->msgType == PBFT_NEWVIEW){
            PBFTNewView newViewMsg;
            if(!newViewMsg.ParseFromArray(body, hdr->msgLen)){
                LOG(ERROR) << "Unable to parse PBFTNewView message";
                continue;
            }
            if (!sigProvider_.verify(hdr, "replica", newViewMsg.primary_id())) {
                LOG(INFO) << "Failed to verify primary replica signature for PBFTNewView!";
                continue;
            }
            const auto& viewChanges = newViewMsg.view_changes();
            const auto& sigs = newViewMsg.view_change_sigs();
            bool success = true;
            for (int i = 0; i < viewChanges.size(); i++) {
                if (!sigProvider_.verify((byte*)viewChanges[i].SerializeAsString().c_str(), viewChanges[i].ByteSizeLong(),
                                         (byte*)sigs[i].c_str(), sigs[i].size(), "replica", viewChanges[i].replica_id())) {
                    LOG(INFO) << "Failed to verify replica signature in new view!";
                    success = false;
                    break;
                }
            }
            if (!success) continue;
            processQueue_.enqueue(msg);
        }else {
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

            if (swapFreq_ && log_->nextSeq % swapFreq_ == 0)
                holdAndSwapCliReq(clientHeader);
            else
                processClientRequest(clientHeader);
        }

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

        else if (hdr->msgType == FALLBACK_TRIGGER) {
            FallbackTrigger fallbackTriggerMsg;

            if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
                return;
            }

            processFallbackTrigger(fallbackTriggerMsg);
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

    if (fallback_) {
        VLOG(6) << "Dropping request due to fallback";
        return;
    }

    if (!checkAndUpdateClientRecord(request))
        return;

    std::string result;
    uint32_t seq;

    bool success = log_->addEntry(clientId, clientSeq, request.req_data(), result);
    seq = log_->nextSeq - 1;

    if (!success) {
        // TODO Handle this more gracefully by queuing requests
        LOG(ERROR) << "Could not add request to log!";
        return;
    }

    std::string digest(log_->getDigest(), log_->getDigest() + SHA256_DIGEST_LENGTH);

    if (VLOG_IS_ON(2)) {
        VLOG(2) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << seq << " client_id=" << clientId
                << " client_seq=" << clientSeq << " instance=" << instance_
                << " digest=" << digest_to_hex(digest).substr(56);
    } else {
        // TODO do some logging here?
    }

    Reply reply;

    reply.set_client_id(clientId);
    reply.set_client_seq(clientSeq);
    reply.set_replica_id(replicaId_);
    reply.set_result(result);
    reply.set_seq(seq);
    reply.set_instance(instance_);
    reply.set_digest(digest);

    sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);

    // Try and commit every CHECKPOINT_INTERVAL replies
    if (seq % CHECKPOINT_INTERVAL == 0) {
        VLOG(2) << "PERF event=checkpoint_start seq=" << seq;

        checkpointCollectors_.tryInitCheckpointCollector(seq, instance_, std::optional<ClientRecords>(clientRecords_));
        // TODO remove execution result here
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
        // If we don't have the matching operation, return false so we don't falsely ack
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
    if (rSeq <= log_->checkpoint.seq) {
        VLOG(4) << "Seq " << rSeq << " is already committed, skipping";
        return;
    }

    checkpointCollectors_.tryInitCheckpointCollector(rSeq, instance_);
    CheckpointCollector &collector = checkpointCollectors_.at(rSeq);
    if (collector.addAndCheckReplyCollection(reply, sig)) {
        const byte *logDigest = log_->getDigest(rSeq);
        std::string appDigest = log_->app_->getDigest(rSeq);
        ClientRecords tmpClientRecords = collector.clientRecords_.value();
        uint32_t instance = instance_;
        // Broadcast commit Message
        dombft::proto::Commit commit;
        commit.set_replica_id(replicaId_);
        commit.set_seq(rSeq);
        commit.set_instance(instance);
        commit.set_log_digest((const char *) logDigest, SHA256_DIGEST_LENGTH);
        commit.set_app_digest(appDigest);

        byte recordDigest[SHA256_DIGEST_LENGTH];
        getRecordsDigest(tmpClientRecords, recordDigest);
        commit.mutable_client_records_set()->set_client_records_digest(recordDigest, SHA256_DIGEST_LENGTH);
        toProtoClientRecords(*commit.mutable_client_records_set(), tmpClientRecords);
        VLOG(1) << "Commit msg record digest: " << digest_to_hex(recordDigest).substr(56);

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
    if (seq <= log_->checkpoint.seq) {
        VLOG(4) << "Seq " << seq << " is already committed, skipping";
        return;
    }

    checkpointCollectors_.tryInitCheckpointCollector(seq, instance_);
    CheckpointCollector &collector = checkpointCollectors_.at(seq);
    // add current commit msg to collector
    if (!collector.addAndCheckCommitCollection(commit, sig)) {
        return;
    }
    LOG(INFO) << "Committing seq=" << seq;
    VLOG(1) << "PERF event=checkpoint_end seq=" << seq;
    // try commit
    bool digest_changed = collector.commitToLog(log_, commit);

    // Unregister fallbackStart timer set by request timeout in slow path
    //  as checkpoint confirms progress
    if(endpoint_->isTimerRegistered(fallbackStartTimer_.get())){
        endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
    }

    if (digest_changed) {
        checkpointClientRecords_.clear();
        getClientRecordsFromProto(commit.client_records_set(), checkpointClientRecords_);
        clientRecords_ = checkpointClientRecords_;

        int rShiftNum = getRightShiftNumWithRecords(checkpointClientRecords_, collector.clientRecords_.value());
        // that is, there is never a left shift
        assert(rShiftNum >= 0);
        if (rShiftNum > 0) {
            VLOG(1) << "Right shift logs by " << rShiftNum << " to align with the checkpoint";
        }
        reapplyEntriesWithRecord(rShiftNum);
    } else {
        checkpointClientRecords_ = collector.clientRecords_.value();
    }

    // if there is overlapping and later checkpoint commits first, skip earlier ones
    checkpointCollectors_.cleanSkippedCheckpointCollectors(seq, instance_);
}

void Replica::processFallbackTrigger(const dombft::proto::FallbackTrigger &msg)
{
    // Ignore repeated fallback triggers
    if (fallback_) {
        LOG(WARNING) << "Received fallback trigger during a fallback from client " << msg.client_id() << " for cseq="
                     << msg.client_seq() << " and instance=" << msg.instance();
        return;
    }

    // TODO if attached request has been executed in previous instance
    // Ignore any messages not for your current instance
    if (msg.instance() < instance_) {
        return;
    }

    if (!msg.has_proof() && endpoint_->isTimerRegistered(fallbackStartTimer_.get())) {
        LOG(WARNING) << "Received redundant fallback trigger due to client side timeout from"
                     << " client " << msg.client_id() << " for cseq=" << msg.client_seq() << " and instance=" << msg.instance();
        return;
    }

    LOG(INFO) << "Received fallback trigger from client " << msg.client_id() << " for cseq=" << msg.client_seq()
              << " and instance=" << msg.instance();

    if (msg.has_proof()) {
        // Proof is verified by verify thread
        LOG(WARNING) << "Fallback trigger has a proof, starting fallback!";

        // TODO we need to broadcast this with the original signature
        // broadcastToReplicas(msg, FALLBACK_TRIGGER);
        startFallback();
    } else {
        endpoint_->RegisterTimer(fallbackStartTimer_.get());
    }
}

void Replica::processFallbackStart(const FallbackStart &msg, std::span<byte> sig)
{
    if ((msg.pbft_view() % replicaAddrs_.size()) != replicaId_) {
        LOG(INFO) << "Received FALLBACK_START for instance " << msg.instance() << " pbft_view "<< msg.pbft_view()<< " where I am not proposer";
        return;
    }

    uint32_t repId = msg.replica_id();
    uint32_t repInstance = msg.instance();
    if(msg.instance() < instance_){
        LOG(INFO) << "Received FALLBACK_START for instance " << msg.instance() << " from replica "
            << repId << " while own is " << instance_;
        return;
    }

    // A corner case where (older instance + pbft_view) targets the same primary and overwrite the newer ones
    if(fallbackHistory_.count(repId) && fallbackHistory_[repId].instance() >= repInstance){
        LOG(INFO) << "Received FALLBACK_START for instance " << repInstance << " pbft_view "<< msg.pbft_view()<< " from replica " << repId << " which is outdated";
        return;
    }
    fallbackHistory_[repId] = msg;
    fallbackHistorySigs_[repId] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received fallbackStart message from replica " << repId;

    if (!isPrimary()) {
        return;
    }

    // First check if we have 2f + 1 fallback start messages for the same instance
    auto numStartMsgs = std::count_if(fallbackHistory_.begin(), fallbackHistory_.end(), [&](auto &startMsg) {
        return startMsg.second.instance() == repInstance;
    });

    if (numStartMsgs == 2 * f_ + 1) {
        doPrePreparePhase(repInstance);
    }
}

// sending helpers
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

bool Replica::verifyCert(const Cert &cert)
{
    if (cert.replies().size() < 2 * f_ + 1) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to "
                  << "cert signatures size" << cert.signatures().size();
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
    if (matchingReplies.size()>1){
        LOG(WARNING) << "Cert has non-matching replies!";
        return false;
    }
    if (matchingReplies.begin()->second.size() < cert.replies().size()){
        LOG(WARNING) << "Cert has replies from the same replica!";
        return false;
    }

    return true;
}

bool Replica::verifyFallbackProof(const Cert &proof){
    if (proof.replies().size() < f_ + 1) {
        LOG(WARNING) << "Received fallback proof of size " << proof.replies().size() << ", which is smaller than f + 1, f=" << f_;
        return false;
    }

    if (proof.replies().size() != proof.signatures().size()) {
        LOG(WARNING) << "Proof replies size " << proof.replies().size() << " is not equal to "
                  << "cert signatures size" << proof.signatures().size();
        return false;
    }

    // check if the replies are matching and no duplicate replies from same replica
    std::map<ReplyKey, std::unordered_set<uint32_t>> matchingReplies;
    // Verify each signature in the proof
    for (int i = 0; i < proof.replies_size(); i++) {
        const Reply &reply = proof.replies()[i];
        const std::string &sig = proof.signatures()[i];
        uint32_t replicaId = reply.replica_id();

        ReplyKey key = {reply.seq(),        reply.instance(), reply.client_id(),
                        reply.client_seq(), reply.digest(),   reply.result()};
        matchingReplies[key].insert(replicaId);
        std::string serializedReply = proof.replies(i).SerializeAsString();
        if (!sigProvider_.verify(
                (byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(), sig.size(), "replica",
                reply.replica_id()
        )) {
            LOG(WARNING) << "Proof failed to verify!";
            return false;
        }
    }

    if (matchingReplies.size()==1){
        LOG(WARNING) << "Proof does not have non-matching replies!";
        return false;
    }
    uint32_t sum = 0;
    for(auto& [_, s]: matchingReplies){
        sum += s.size();
    }

    if (sum < proof.replies().size()){
        LOG(WARNING) << "Proof has replies from the same replica!";
        return false;
    }
    return true;
}

bool Replica::verifyFallbackProposal(const FallbackProposal& proposal){
    std::vector<std::tuple<byte*, uint32_t >> logSigs;
    for (auto &sig : proposal.signatures()) {
        logSigs.emplace_back((byte*)sig.data(), sig.length());
    }
    uint32_t ind = 0;
    for (auto &log : proposal.logs()) {
        std::string logStr = log.SerializeAsString();
        byte *logBuffer = (byte*)logStr.data();
        byte* logSig = std::get<0>(logSigs[ind]);
        uint32_t logSigLen = std::get<1>(logSigs[ind]);
        ind++;
        if (!sigProvider_.verify(logBuffer, logStr.length(),logSig, logSigLen, "replica", log.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature in proposal!";
            return false;
        }
    }
    return true;
}

bool Replica::verifyViewChange(const PBFTViewChange &viewChangeMsg){
    // verify prepares
    const auto& prepares = viewChangeMsg.prepares();
    const auto& sigs = viewChangeMsg.prepare_sigs();

    if(prepares.empty() && viewChangeMsg.instance()== UINT32_MAX){
        LOG(INFO) << "View Change with no previous agreed prepares";
        return true;
    }
    std::unordered_set<uint32_t> insts;
    std::unordered_set<std::string> proposal_digests;
    for (int i = 0; i < prepares.size(); i++) {
        if (!sigProvider_.verify((byte*)prepares[i].SerializeAsString().c_str(), prepares[i].ByteSizeLong(),
                                    (byte*)sigs[i].c_str(), sigs[i].size(), "replica", prepares[i].replica_id())) {
            LOG(INFO) << "Failed to verify replica signature in view change!";
            return false;
        }
        insts.emplace(prepares[i].instance());
        proposal_digests.emplace(prepares[i].proposal_digest());
    }
    if(insts.size() != 1 || proposal_digests.size() != 1){
        LOG(INFO) << "View Change with inconsistent prepares and proposals from replica " << viewChangeMsg.replica_id() << " instance=" << viewChangeMsg.instance() <<" pbf_view=" << viewChangeMsg.pbft_view();
        return false;
    }
    if(insts.find(viewChangeMsg.instance()) == insts.end()){
        LOG(INFO) << "View Change with different instance";
        return false;
    }
    const FallbackProposal& proposal = viewChangeMsg.proposal();
    if (!verifyFallbackProposal(proposal)) {
        LOG(INFO) << "Failed to verify fallback proposal!";
        return false;
    }
    return true;
}

void Replica::startFallback()
{
    assert(!fallback_);
    fallback_ = true;
    LOG(INFO) << "Starting fallback on instance " << instance_;

    // It is possible that some earlier reqs are timedout on clients but not yet timedout here
    if(endpoint_->isTimerRegistered(fallbackStartTimer_.get())){
        endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
    }

    // Start fallback timer to change primary if timeout
    if (endpoint_->isTimerRegistered(fallbackTimer_.get())) {
        endpoint_->ResetTimer(fallbackTimer_.get());
    } else {
        endpoint_->RegisterTimer(fallbackTimer_.get());
    }

    VLOG(1) << "PERF event=fallback_start replica_id=" << replicaId_ << " seq=" << log_->nextSeq
            << " instance=" << instance_ << " pbft_view=" << pbftView_;

    // Extract log into start fallback message
    FallbackStart fallbackStartMsg;
    fallbackStartMsg.set_instance(instance_);
    fallbackStartMsg.set_replica_id(replicaId_);
    fallbackStartMsg.set_pbft_view(pbftView_);
    log_->toProto(fallbackStartMsg);
    byte recordDigest[SHA256_DIGEST_LENGTH];
    getRecordsDigest(checkpointClientRecords_, recordDigest);
    toProtoClientRecords(*fallbackStartMsg.mutable_client_records_set(), checkpointClientRecords_);

    uint32_t primaryId = getPrimary();
    LOG(INFO) << "Sending FALLBACK_START to PBFT primary replica " << primaryId;
    sendMsgToDst(fallbackStartMsg, FALLBACK_START, replicaAddrs_[primaryId]);
    LOG(INFO) << "DUMP start fallback instance=" << instance_ << " " << *log_;
}

void Replica::replyFromLogEntry(Reply &reply, uint32_t seq)
{
    std::shared_ptr<::LogEntry> entry = log_->getEntry(seq);   // TODO better namespace

    reply.set_client_id(entry->client_id);
    reply.set_client_seq(entry->client_seq);
    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_);
    reply.set_result(entry->result);
    reply.set_seq(entry->seq);
    reply.set_digest(entry->digest, SHA256_DIGEST_LENGTH);
}

void Replica::finishFallback()
{
    assert(fallbackProposal_.has_value());

    FallbackProposal &history = fallbackProposal_.value();
    LOG(INFO) << "Applying fallback with primary's instance=" << history.instance() << " from own instance=" << instance_;
    instance_ = history.instance();
    endpoint_->UnRegisterTimer(fallbackTimer_.get());

    LogSuffix logSuffix;
    getLogSuffixFromProposal(history, logSuffix);

    logSuffix.replicaId = replicaId_;
    logSuffix.instance = instance_;

    applySuffixToLog(logSuffix, log_);
    clientRecords_ = logSuffix.clientRecords;

    VLOG(1) << "PERF event=fallback_end replica_id=" << replicaId_ << " seq=" << log_->nextSeq
            << " instance=" << instance_ << " pbft_view=" << pbftView_;
    LOG(INFO) << "DUMP finish fallback instance=" << instance_ << " " << *log_;
    instance_++;

    LOG(INFO) << "Instance updated to " << instance_ << " and pbft_view to " << pbftView_;

    FallbackSummary summary;
    std::set<int> clients;

    summary.set_instance(instance_);
    summary.set_replica_id(replicaId_);

    uint32_t seq = log_->checkpoint.seq;
    for (; seq < log_->nextSeq; seq++) {
        std::shared_ptr<::LogEntry> entry = log_->getEntry(seq);   // TODO better namespace
        FallbackReply reply;

        clients.insert(entry->client_id);

        reply.set_client_id(entry->client_id);
        reply.set_client_seq(entry->client_seq);
        reply.set_seq(entry->seq);

        (*summary.add_replies()) = reply;
    }

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(summary, MessageType::FALLBACK_SUMMARY);
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

    // TODO make this only send to clients that need it
    LOG(INFO) << "Sending fallback summary for instance=" << instance_;
    for (auto &addr : clientAddrs_) {
        endpoint_->SendPreparedMsgTo(addr);
    }

    fallback_ = false;
    if(viewChange_){
        viewChangeInst_ = instance_ + viewChangeFreq_;
        viewChange_ = false;
    }
    fallbackProposal_.reset();
    fallbackPrepares_.clear();
    fallbackPBFTCommits_.clear();

    // TODO(Hao): since the fallback is PBFT, we can simply set the checkpoint here already
    checkpointCollectors_.tryInitCheckpointCollector(
        log_->nextSeq - 1, instance_, std::optional<ClientRecords>(clientRecords_)
    );
    Reply reply;
    replyFromLogEntry(reply, log_->nextSeq - 1);
    broadcastToReplicas(reply, MessageType::REPLY);
}

// dummy fallback PBFT

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
    for (auto &startMsg : fallbackHistory_) {
        if (startMsg.second.instance() != instance )
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
    LOG(INFO) << "Prepare for instance=" << instance_ << " replicaId=" << replicaId_;
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
    if(viewChangeByCommit() && commitLocalInViewChange_){
        sendMsgToDst(cmt, PBFT_COMMIT, replicaAddrs_[replicaId_]);
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
    if(msg.pbft_view()!=pbftView_){
        LOG(INFO) << "Received preprepare from replicaId=" << msg.primary_id() << " for instance=" << msg.instance() << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if(getPrimary()!=msg.primary_id()){
        LOG(INFO) << "Received fallback preprepare from non-primary replica " << msg.primary_id() << ". Current selected primary is " << getPrimary();
        return;
    }

    LOG(INFO) << "PrePrepare RECEIVED for instance=" << msg.instance() << " from replicaId=" << msg.primary_id();
    // accepts the proposal as long as it's from the primary
    fallbackProposal_ = msg.proposal();
    memcpy(proposalDigest_, msg.proposal_digest().c_str(), SHA256_DIGEST_LENGTH);

    if(viewChangeByPrepare()){
        LOG(INFO) << "Prepare message held to cause timeout in prepare phase for view change";
        return;
    }
    doPreparePhase();
}

void Replica::processPrepare(const PBFTPrepare &msg, std::span<byte> sig)
{
    uint32_t inInst = msg.instance();
    if(msg.pbft_view()!=pbftView_){
        LOG(INFO) << "Received prepare from replicaId=" << msg.replica_id() << " for instance=" <<  inInst << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if ( inInst < instance_ && viewPrepared_) {
        LOG(INFO) << "Received old fallback prepare from instance=" <<  inInst << " own instance is "
                  << instance_;
        return;
    }

    if( fallbackPrepares_.count(msg.replica_id()) && fallbackPrepares_[msg.replica_id()].instance() >  inInst && fallbackPrepares_[msg.replica_id()].pbft_view()  == msg.pbft_view()){
        LOG(INFO) << "Old prepare received from replicaId=" << msg.replica_id() << " for instance=" <<  inInst;
        return;
    }
    fallbackPrepares_[msg.replica_id()] = msg;
    fallbackPrepareSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());
    LOG(INFO) << "Prepare RECEIVED for instance=" <<  inInst << " from replicaId=" << msg.replica_id();
    // skip if already prepared for it
    // note: if viewPrepared_==false, then viewChange_==true
    if (viewPrepared_ && preparedInstance_ == inInst) {
        return;
    }
    if(!fallbackProposal_.has_value() || fallbackProposal_.value().instance() <  inInst){
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process prepare";
        return;
    }
    // Make sure the Prepare msgs are for the corresponding PrePrepare msg
    auto numMsgs = std::count_if(fallbackPrepares_.begin(), fallbackPrepares_.end(), [this](auto &curMsg) {
        return curMsg.second.instance() == fallbackProposal_.value().instance() && memcmp(curMsg.second.proposal_digest().c_str(), proposalDigest_, SHA256_DIGEST_LENGTH) == 0;
    });
    if (numMsgs < 2 * f_ + 1) {
        return;
    }
    // Store PBFT states for potential view change
    preparedInstance_ = fallbackProposal_.value().instance();
    viewPrepared_ = true;
    pbftState_.proposal = fallbackProposal_.value();
    memcpy(pbftState_.proposal_digest, proposalDigest_, SHA256_DIGEST_LENGTH);
    pbftState_.prepares.clear();
    for(const auto& [repId, prepare]:fallbackPrepares_){
        if (prepare.instance() == preparedInstance_) {
            pbftState_.prepares[repId] = prepare;
            pbftState_.prepareSigs[repId] = fallbackPrepareSigs_[repId];
        }
    }
    LOG(INFO) << "Prepare received from 2f + 1 replicas, agreement reached for instance=" << preparedInstance_ << " pbft_view=" << pbftView_;

    if(viewChangeByCommit()){
        if(commitLocalInViewChange_){
            LOG(INFO) << "Commit message only send to itself to commit locally to advance to next instance";
            doCommitPhase();
        }else{
            LOG(INFO) << "Commit message held to cause timeout in commit phase for view change";
        }
        return;
    }
    doCommitPhase();
}

void Replica::processPBFTCommit(const PBFTCommit &msg)
{
    uint32_t inInst = msg.instance();
    if(msg.pbft_view()!=pbftView_){
        LOG(INFO) << "Received commit from replicaId=" << msg.replica_id() << " for instance=" <<  inInst << " with different pbft_view=" << msg.pbft_view();
        return;
    }
    if ( inInst < instance_ && !viewChange_) {
        LOG(INFO) << "Received old fallback commit from instance=" <<  inInst << " own instance is "
                  << instance_;
        return;
    }
    if(fallbackPBFTCommits_.count(msg.replica_id()) && fallbackPBFTCommits_[msg.replica_id()].instance() > inInst && fallbackPBFTCommits_[msg.replica_id()].pbft_view() == msg.pbft_view()){
        LOG(INFO) << "Old commit received from replicaId=" << msg.replica_id() << " for instance=" <<  inInst;
        return;
    }
    fallbackPBFTCommits_[msg.replica_id()] = msg;
    LOG(INFO) << "PBFTCommit RECEIVED for instance=" <<  inInst << " from replicaId=" << msg.replica_id();

    if(!fallbackProposal_.has_value() || fallbackProposal_.value().instance() <  inInst){
        LOG(INFO) << "PrePrepare not received yet, wait till it arrives to process commit";
        return;
    }

    if (preparedInstance_==UINT32_MAX || preparedInstance_ != inInst || !viewPrepared_){
        LOG(INFO) << "Not prepared for it, skipping commit!";
        return;
    }
    auto numMsgs = std::count_if(fallbackPBFTCommits_.begin(), fallbackPBFTCommits_.end(), [this](auto &curMsg) {
        return curMsg.second.instance() == preparedInstance_ && memcmp(curMsg.second.proposal_digest().c_str(), proposalDigest_, SHA256_DIGEST_LENGTH) == 0;
    });
    if (numMsgs < 2 * f_ + 1) {
        return;
    }
    LOG(INFO) << "Commit received from 2f + 1 replicas, Committed!";
    finishFallback();

}


void Replica::startViewChange(){
    pbftView_++;
    fallback_ = true;
    viewChange_=true;
    viewPrepared_=false;
    VLOG(1) << "PERF event=viewchange_start replica_id=" << replicaId_ << " seq=" << log_->nextSeq
            << " instance=" << instance_ << " pbft_view=" << pbftView_;
    LOG(INFO) << "Starting ViewChange on instance " << instance_ << " pbft_view " << pbftView_;

    PBFTViewChange viewChange;
    viewChange.set_replica_id(replicaId_);
    viewChange.set_instance(preparedInstance_);
    viewChange.set_pbft_view(pbftView_);

    // Add the latest quorum of prepares and sigs
    if(preparedInstance_!=UINT32_MAX){
        for (const auto& [repId, prepare]:pbftState_.prepares){
            *(viewChange.add_prepares()) = prepare;
            *(viewChange.add_prepare_sigs()) = pbftState_.prepareSigs[repId];
        }
        viewChange.set_proposal_digest(pbftState_.proposal_digest, SHA256_DIGEST_LENGTH);
        viewChange.mutable_proposal()->CopyFrom(pbftState_.proposal);
    }

    broadcastToReplicas(viewChange, PBFT_VIEWCHANGE);
}
void Replica::processPBFTViewChange(const PBFTViewChange &msg, std::span<byte> sig){
    // No instance checking as view change is not about instance
    uint32_t inViewNum = msg.pbft_view();
    if(inViewNum < pbftView_) {
        LOG(INFO) << "Received outdated view change from pbft_view=" << inViewNum << " own pbft_view is "
                  << pbftView_;
        return;
    }

    if(pbftViewChanges_.count(msg.replica_id()) && pbftViewChanges_[msg.replica_id()].pbft_view() > inViewNum){
        LOG(INFO) << "Outdated view change received from replicaId=" << msg.replica_id() << " for pbft_view=" << inViewNum;
        return;
    }

    pbftViewChanges_[msg.replica_id()] = msg;
    pbftViewChangeSigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "ViewChange RECEIVED for pbft_view=" << inViewNum << " from replicaId=" << msg.replica_id();

    auto numMsgs = std::count_if(pbftViewChanges_.begin(), pbftViewChanges_.end(), [this,inViewNum](auto &curMsg) {
        return curMsg.second.pbft_view() == inViewNum;
    });

    if (numMsgs != 2 * f_ + 1) {
        return;
    }
    LOG(INFO) << "ViewChange for view " << inViewNum<<" received from 2f + 1 replicas!";

    // non-primary replicas collect view change msgs for
    // 1. delaying timer setting for better liveness (avoid frequent view changes)
    // 2. check if the majority has a larger view# and start view change if so (avoid starting view change too late)
    if(endpoint_->isTimerRegistered(fallbackTimer_.get())){
        endpoint_->ResetTimer(fallbackTimer_.get());
    }else{
        endpoint_->RegisterTimer(fallbackTimer_.get());
    }
    if(inViewNum > pbftView_){
        LOG(INFO) << "Majority has a larger view number, starting a new view change for the major view";
        pbftView_ = inViewNum - 1; // will add 1 back in startViewChange
        startViewChange();
    }

    if(!isPrimary()) {
        pbftViewChanges_.clear();
        pbftViewChangeSigs_.clear();
        return;
    }

    // primary search for the latest prepared instance
    uint32_t maxInstance = UINT32_MAX;
    // vc.instance can be UINT32_MAX if no prepares, which is fine
    for (const auto& [_, vc] : pbftViewChanges_){
        if(vc.pbft_view()!=inViewNum){
            continue;
        }
        if(maxInstance==UINT32_MAX && vc.instance() < maxInstance){
            maxInstance = vc.instance();
        }else if(maxInstance!=UINT32_MAX && vc.instance() > maxInstance){
            maxInstance = vc.instance();
        }
    }
    LOG(INFO) << "Found the latest prepared instance=" << maxInstance << " for pbft_view=" << inViewNum;

    PBFTNewView newView;
    newView.set_primary_id(replicaId_);
    newView.set_pbft_view(inViewNum);
    newView.set_instance(maxInstance);

    for (const auto& [repId, vc] : pbftViewChanges_){
        newView.add_view_changes()->CopyFrom(vc);
        *(newView.add_view_change_sigs()) = pbftViewChangeSigs_[repId];
    }

    broadcastToReplicas(newView, PBFT_NEWVIEW);
}

void Replica::processPBFTNewView(const PBFTNewView &msg){
    // TODO(Hao) here should be some checks for view change msgs in NewView, skip for now..
    if(pbftView_ > msg.pbft_view()){
        LOG(INFO) << "Received outdated new view from pbft_view=" << msg.pbft_view() << " own pbft_view is "
                  << pbftView_;
        return;
    }

    const auto& viewChanges = msg.view_changes();
    std::unordered_map<uint32_t, uint32_t> views;
    PBFTViewChange maxVC;
    for(const auto& vc: viewChanges){
        views[vc.pbft_view()] ++;
        if(vc.instance() > maxVC.instance()){
            maxVC = vc;
        }
    }
    if(maxVC.pbft_view()!=msg.pbft_view() || maxVC.instance()!=msg.instance()){
        LOG(INFO) << "Replica obtains a different choice of instance=" << maxVC.instance() << " and pbft_view=" << maxVC.pbft_view();
    }
    if(views[maxVC.pbft_view()] < 2 * f_ + 1){
        LOG(INFO) << "The view number " << maxVC.pbft_view() << " does not have a 2f + 1 quorum";
        return;
    }
    LOG(INFO) << "Received NewView for pbft_view=" << msg.pbft_view() << " with prepared instance=" << msg.instance();
    pbftView_ = msg.pbft_view();
    // in case it is not in view change already. Not quite sure this is correct way tho
    viewChange_ = true;
    fallback_ = true;
    viewPrepared_=false;
    fallbackProposal_.reset();
    fallbackPrepares_.clear();
    fallbackPBFTCommits_.clear();

    // TODO(Hao): test this corner case later
    if( msg.instance() == UINT32_MAX){
        LOG(INFO) << "No previously prepared instance in new vew, go back to normal state. EXIT FOR NOW";
        assert(msg.instance() != UINT32_MAX);
        return;
    }
    fallbackProposal_ = maxVC.proposal();
    memcpy(proposalDigest_, maxVC.proposal_digest().c_str(), SHA256_DIGEST_LENGTH);
    // set view change param to true to bypass instance check.
    doPreparePhase();
}

void Replica::getProposalDigest(byte* digest, const FallbackProposal &proposal){
    // use signatures as digest
    std::string digestStr;
    for (const auto& sig : proposal.signatures()){
        digestStr += sig;
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, digestStr.c_str(), digestStr.size());
    SHA256_Final(digest, &ctx);
}

bool Replica::checkAndUpdateClientRecord(const ClientRequest &clientHeader)
{
    uint32_t clientId = clientHeader.client_id();
    uint32_t clientSeq = clientHeader.client_seq();
    uint32_t clientInstance = clientHeader.instance();

    ClientRecord &cliRecord = clientRecords_[clientId];
    cliRecord.instance_ = std::max(clientInstance, cliRecord.instance_);

    if (clientInstance < instance_) {
        LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                  << " due to stale instance! Sending blank reply to catch client up";
        // Send blank request to catch up the client
        Reply reply;
        reply.set_replica_id(replicaId_);
        reply.set_client_id(clientId);
        reply.set_instance(instance_);

        sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);
        return false;
    }

    if (!log_->canAddEntry()) {
        LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq << " due to log full!";
        return false;
    }

    if (!cliRecord.updateRecordWithSeq(clientSeq)) {
        LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                  << " due to duplication! Send reply to client";

        uint32_t instance = instance_;
        byte logDigest[SHA256_DIGEST_LENGTH];
        memcpy(logDigest, log_->checkpoint.logDigest, SHA256_DIGEST_LENGTH);
        Reply reply;
        // TODO(Hao): is providing these info enough for client?
        //  use checkpoint digest for now
        reply.set_client_id(clientId);
        reply.set_client_seq(clientSeq);
        reply.set_replica_id(replicaId_);
        reply.set_retry(true);
        reply.set_digest(logDigest, SHA256_DIGEST_LENGTH);
        reply.set_instance(instance);

        LOG(INFO) << "Sending retry reply back to client " << clientId;
        sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId]);
        return false;
    }

    return true;
}

void Replica::reapplyEntriesWithRecord(uint32_t rShiftNum)
{
    uint32_t startingSeq = log_->checkpoint.seq + 1;
    if (rShiftNum) {
        log_->rightShiftEntries(startingSeq - rShiftNum, rShiftNum);
    }
    log_->app_->abort(startingSeq - 1);
    uint32_t curSeq = startingSeq;
    std::vector<std::shared_ptr<::LogEntry>> temp(log_->nextSeq - startingSeq);
    for (uint32_t s = startingSeq; s < log_->nextSeq; s++) {
        std::shared_ptr<::LogEntry> entry = log_->getEntry(s);
        uint32_t clientId = entry->client_id;
        uint32_t clientSeq = entry->client_seq;
        std::string clientReq = entry->request;

        ClientRecord &cliRecord = clientRecords_[clientId];
        cliRecord.instance_ = instance_;
        if (!cliRecord.updateRecordWithSeq(clientSeq)) {
            LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                      << " due to duplication in reapplying with record!";
            continue;
        }

        log_->app_->execute(clientReq, curSeq);

        temp[curSeq - startingSeq] = entry;
        entry->seq = curSeq;
        auto prevDigest = curSeq == startingSeq ? log_->checkpoint.logDigest : temp[curSeq - startingSeq - 1]->digest;

        entry->updateDigest(prevDigest);

        VLOG(1) << "PERF event=update_digest seq=" << curSeq << " digest=" << digest_to_hex(entry->digest).substr(56)
                << " c_id=" << clientId << " c_seq=" << clientSeq
                << " prevDigest=" << digest_to_hex(prevDigest).substr(56);
        curSeq++;
    }

    if (curSeq != log_->nextSeq) {
        log_->nextSeq = curSeq;
        for (uint32_t i = startingSeq; i < curSeq; i++) {
            log_->setEntry(i, temp[i - startingSeq]);
        }
    }
}

}   // namespace dombft