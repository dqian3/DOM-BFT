#include "replica.h"

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include <openssl/pem.h>
#include <sstream>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t swapFreq)
    : replicaId_(replicaId)
    , instance_(0)
    , f_(config.replicaIps.size() / 3)
    , swapFreq_(swapFreq)
    , sigProvider_()
    , threadpool_(config.replicaNumWorkerThreads)
{
    // TODO check for config errors
    std::string replicaIp = config.replicaIps[replicaId];
    LOG(INFO) << "replicaIP=" << replicaIp;

    std::string bindAddress = config.receiverLocal ? "0.0.0.0" : replicaIp;
    LOG(INFO) << "bindAddress=" << bindAddress;

    int replicaPort = config.replicaPort;
    LOG(INFO) << "replicaPort=" << replicaPort;

    std::string replicaKey = config.replicaKeysDir + "/replica" + std::to_string(replicaId_) + ".pem";
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

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true,
                                                          replicaAddrs_[replicaId]);
    } else {
        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);

        /** Store all replica addrs */
        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
        }

        receiverAddr_ = Address(config.receiverIps[replicaId_], config.receiverPort);

        /** Store all client addrs */
        for (uint32_t i = 0; i < config.clientIps.size(); i++) {
            clientAddrs_.push_back(Address(config.clientIps[i], config.clientPort));
        }
    }

    MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender){
        this->handleMessage(msgHdr, msgBuffer, sender, *sender == replicaAddrs_[replicaId_]);
    };

    endpoint_->RegisterMsgHandler(handler);

    fallbackStartTimer_ = std::make_unique<Timer>(
        [this](void *ctx, void *endpoint) {
            endpoint_->UnRegisterTimer(fallbackStartTimer_.get());
            this->startFallback();
        },
        config.replicaFallbackStartTimeout, this);

    fallbackTimer_ = std::make_unique<Timer>(
        [this](void *ctx, void *endpoint) {
            LOG(INFO) << "Fallback for instance=" << instance_ << " failed!";
            this->startFallback();
        },
        config.replicaFallbackTimeout, this);

    endpoint_->RegisterSignalHandler([&]() {
        LOG(INFO) << "Received interrupt signal!";
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
    LOG(INFO) << "Starting main event loop...";
    endpoint_->LoopRun();

    LOG(INFO) << "Finishing main event loop...";
}

#if PROTOCOL == DOMBFT
void Replica::handleMessage(MessageHeader *hdr, byte *body, Address *sender, const bool skipVerify)
{
    if (hdr->msgLen < 0) {
        return;
    }

    // Note, here we skip verifying the underlying client message,
    // since this is offloaded to the receiver process

    if (hdr->msgType == MessageType::DOM_REQUEST) {
        DOMRequest domHeader;
        ClientRequest clientHeader;

        if (*sender != receiverAddr_) {
            LOG(ERROR) << "Received DOM_REQUEST message from non-receiever!";
            return;
        }

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

        if (fallback_) {
            VLOG(6) << "Dropping request due to fallback";
            return;
        }

        // TODO(Hao) should this be in handleClientRequest?
        if (!checkAndUpdateClientRecord(clientHeader)) return;

        if (swapFreq_ && log_->nextSeq % swapFreq_ == 0)
            holdAndSwapCliReq(clientHeader);
        else
            handleClientRequest(clientHeader);
    }

    if (hdr->msgType == CERT) {
        Cert cert;

        if (!cert.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CERT message";
            return;
        }

        handleCert(cert);
    }

    if (hdr->msgType == REPLY) {
        Reply replyHeader;

        if (!replyHeader.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DOM_REQUEST message";
            return;
        }

        if (!skipVerify && !sigProvider_.verify(hdr, body, "replica", replyHeader.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        handleReply(replyHeader, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    if (hdr->msgType == COMMIT) {
        Commit commitMsg;

        if (!commitMsg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DOM_REQUEST message";
            return;
        }

        if (!skipVerify && !sigProvider_.verify(hdr, body, "replica", commitMsg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        handleCommit(commitMsg, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    if (hdr->msgType == FALLBACK_TRIGGER) {
        FallbackTrigger fallbackTriggerMsg;

        if (!fallbackTriggerMsg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
            return;
        }

        // Ignore any messages not for your current instance
        if (fallbackTriggerMsg.instance() != instance_) {
            return;
        }

        if (endpoint_->isTimerRegistered(fallbackStartTimer_.get())) {
            LOG(INFO) << "Received fallback trigger again!";
            return;
        }

        if (!skipVerify &&  !sigProvider_.verify(hdr, body, "client", fallbackTriggerMsg.client_id())) {
            LOG(INFO) << "Failed to verify client signature of c_id=" << fallbackTriggerMsg.client_id();
            return;
        }

        LOG(INFO) << "Received fallback trigger from " << fallbackTriggerMsg.client_id()
                  << " for cseq=" << fallbackTriggerMsg.client_seq()
                  << " and instance=" << fallbackTriggerMsg.instance();

        // TODO if attached request has been executed in another view,
        // send result back

        if (fallbackTriggerMsg.has_proof()) {
            // TODO verify proof
            LOG(INFO) << "Fallback trigger has a proof, starting fallback!";
            broadcastToReplicas(fallbackTriggerMsg, FALLBACK_TRIGGER);
            startFallback();
        } else {
            endpoint_->RegisterTimer(fallbackStartTimer_.get());
        }
    }

    if (hdr->msgType == FALLBACK_START) {
        FallbackStart msg;

        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse FALLBACK_TRIGGER message";
            return;
        }

        if ((msg.instance() % replicaAddrs_.size()) != replicaId_) {
            LOG(INFO) << "Received FALLBACK_START for instance " << msg.instance() << " where I am not leader";
            return;
        }

        if (!skipVerify &&  !sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        // TODO handle case where instance is higher
        handleFallbackStart(msg, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    if (hdr->msgType == DUMMY_PREPREPARE) {
        FallbackPrePrepare msg;

        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DUMMY_PREPREPARE message";
            return;
        }

        if (!skipVerify && !sigProvider_.verify(hdr, body, "replica", msg.primary_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        if (msg.instance() < instance_) {
            LOG(INFO) << "Received old fallback preprepare from instance=" << msg.instance() << " own instance is "
                      << instance_;
            return;
        }
        handlePrePrepare(msg);
    }

    if (hdr->msgType == DUMMY_PREPARE) {
        FallbackPrepare msg;

        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DUMMY_PREPARE message";
            return;
        }

        if (!skipVerify && !sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        if (msg.instance() < instance_) {
            LOG(INFO) << "Received old fallback prepare from instance=" << msg.instance() << " own instance is "
                      << instance_;
            return;
        }
        handlePrepare(msg);
    }

    if (hdr->msgType == DUMMY_COMMIT) {
        FallbackPBFTCommit msg;

        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DUMMY_COMMIT message";
            return;
        }

        if (!skipVerify && !sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        if (msg.instance() < instance_) {
            LOG(INFO) << "Received old fallback commit from instance=" << msg.instance() << " own instance is "
                      << instance_;
            return;
        }
        handlePBFTCommit(msg);
    }
}

#else   // if not DOM_BFT
void Replica::handleMessage(MessageHeader *hdr, byte *body, Address *sender)
{
    if (hdr->msgLen < 0) {
        return;
    }

    if (hdr->msgType == CLIENT_REQUEST) {
        // Only leader should get client requests for now
        if (replicaId_ != 0) {
            LOG(ERROR) << "Non leader got CLIENT_REQUEST in dummy PBFT/ZYZZYVA";
            return;
        }

        ClientRequest clientHeader;

        if (!clientHeader.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            return;
        }

        if (!sigProvider_.verify(hdr, body, "client", clientHeader.client_id())) {
            LOG(INFO) << "Failed to verify client signature!";
            return;
        }

        DummyProto prePrepare;
        prePrepare.set_client_id(clientHeader.client_id());
        prePrepare.set_client_seq(clientHeader.client_seq());
        prePrepare.set_stage(0);
        prePrepare.set_replica_id(replicaId_);
        prePrepare.set_replica_seq(seq_);

        VLOG(3) << "Leader preprepare " << clientHeader.client_id() << ", " << clientHeader.client_seq()
                << " at sequence number " << seq_;

        seq_++;

        broadcastToReplicas(prePrepare, MessageType::DUMMY_PROTO);
    }

    if (hdr->msgType == DUMMY_PROTO) {
        DummyProto msg;
        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DUMMY_PROTO message";
            return;
        }

        if (!sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

#if PROTOCOL == ZYZ

        // Handle client request currently just replies back to client,
        // Use that here as a hack lol.
        VLOG(2) << "Replica " << replicaId_ << " got preprepare for " << msg.replica_seq();
        ClientRequest dummyReq;
        dummyReq.set_client_id(msg.client_id());
        dummyReq.set_client_seq(msg.client_seq());
        handleClientRequest(dummyReq, msg.replica_seq());

#elif PROTOCOL == PBFT
        std::pair<int, int> key = {msg.client_id(), msg.client_seq()};

        if (msg.stage() == 0) {
            VLOG(2) << "Replica " << replicaId_ << " got preprepare for " << msg.replica_seq();
            msg.set_stage(1);
            msg.set_replica_id(replicaId_);
            broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
        } else if (msg.stage() == 1) {
            prepareCount[key]++;

            VLOG(4) << "Prepare received from " << msg.replica_id() << " now " << prepareCount[key] << " out of "
                    << replicaAddrs_.size() / 3 * 2 + 1;

            if (prepareCount[key] == replicaAddrs_.size() / 3 * 2 + 1) {
                VLOG(2) << "Replica " << replicaId_ << " is prepared on " << msg.replica_seq();

                msg.set_stage(2);
                msg.set_replica_id(replicaId_);

                broadcastToReplicas(msg, MessageType::DUMMY_PROTO);
            }
        } else if (msg.stage() == 2) {
            commitCount[key]++;

            VLOG(4) << "Commit received from " << msg.replica_id() << " now " << commitCount[key] << " out of "
                    << replicaAddrs_.size() / 3 * 2 + 1;

            if (commitCount[key] == replicaAddrs_.size() / 3 * 2 + 1) {
                VLOG(2) << "Replica " << replicaId_ << " is committed on " << msg.replica_seq();

                ClientRequest dummyReq;
                dummyReq.set_client_id(msg.client_id());
                dummyReq.set_client_seq(msg.client_seq());
                handleClientRequest(dummyReq, msg.replica_seq());
            }
        }
#endif
    }
}
#endif

void Replica::handleClientRequest(const ClientRequest &request)
{
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();
    VLOG(2) << "Received request from client " << clientId << " with seq " << clientSeq;

    if (clientId < 0 || clientId > clientAddrs_.size()) {
        LOG(ERROR) << "Invalid client id" << clientId;
        return;
    }

    std::string result;
    uint32_t seq;

    {
        // Check lock contention time here
        uint64_t now = GetMicrosecondTimestamp();
        std::lock_guard<std::mutex> guard(replicaStateMutex_);
        VLOG(5) << "Waited " << GetMicrosecondTimestamp() - now << " usec for lock on log apply";

        bool success = log_->addEntry(clientId, clientSeq, request.req_data(), result);
        seq = log_->nextSeq - 1;

        if (!success) {
            // TODO Handle this more gracefully by queuing requests
            LOG(ERROR) << "Could not add request to log!";
            return;
        }
    }

    uint32_t instance = instance_;
    std::string digest(log_->getDigest(), log_->getDigest() + SHA256_DIGEST_LENGTH);

    LOG(ERROR) << "PERF event=spec_execute replica_id=" << replicaId_ << " seq=" << seq << " client_id=" << clientId
               << " client_seq=" << clientSeq << " instance=" << instance_
               << " digest=" << digest_to_hex(digest).substr(56);

    ClientRecords tmpClientRecords = clientRecords_;
    // TODO, only queue signing tasks? Then we don't have to worry about locking replica state
    threadpool_.enqueueTask([=, this](byte *buffer) {
        Reply reply;

        reply.set_client_id(clientId);
        reply.set_client_seq(clientSeq);
        reply.set_replica_id(replicaId_);
        reply.set_result(result);
        reply.set_fast(true);
        reply.set_seq(seq);
        reply.set_instance(instance);
        reply.set_digest(digest);

    LOG(INFO) << "Sending reply back to client " << clientId;
    sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId], buffer);

    // Try and commit every CHECKPOINT_INTERVAL replies
    if (seq % CHECKPOINT_INTERVAL == 0) {
        VLOG(1) << "PERF event=checkpoint_start seq=" << seq;
        {
            std::lock_guard<std::mutex> guard(replicaStateMutex_);
            // an intermediate record is needed as current checkpointing can be interrupted by fallback
            // so only when the checkpoint is finished, the checkpoint record is updated
            intermediateCheckpointClientRecords_ = tmpClientRecords;
            checkpointSeq_ = seq;
            checkpointCert_.reset();
        }
        // TODO remove execution result here
        broadcastToReplicas(reply, MessageType::REPLY, buffer);
    }
    });
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
    handleClientRequest(request);
    handleClientRequest(heldRequest_.value());
    VLOG(2) << "Swapped requests (" << clientId << ", " << clientSeq << ") and (" << heldRequest_->client_id() << ", "
            << heldRequest_->client_seq() << ")";
    heldRequest_.reset();
}

void Replica::handleCert(const Cert &cert)
{
    // TODO make sure this works
    threadpool_.enqueueTask([=, this](byte *buffer) {
        if (!verifyCert(cert)) {
            return;
        }

        const Reply &r = cert.replies()[0];
        CertReply reply;

        {
            std::lock_guard<std::mutex> guard(replicaStateMutex_);
            if (cert.instance() < instance_) {
                VLOG(2) << "Received stale cert with instance " << cert.instance() << " < " << instance_
                        << " for seq=" << r.seq() << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
                return;
            }

            log_->addCert(r.seq(), cert);

            reply.set_replica_id(replicaId_);
            reply.set_instance(instance_);
        }

        reply.set_client_id(r.client_id());
        reply.set_client_seq(r.client_seq());
        reply.set_seq(r.seq());

        VLOG(3) << "Sending cert ack for " << reply.client_id() << ", " << reply.client_seq() << " to "
                << clientAddrs_[reply.client_id()].ip();

        sendMsgToDst(reply, MessageType::CERT_REPLY, clientAddrs_[reply.client_id()], buffer);
    });
}

void Replica::handleReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
    std::lock_guard<std::mutex> guard(replicaStateMutex_);

    VLOG(3) << "Processing reply from replica " << reply.replica_id() << " for seq " << reply.seq();
    checkpointReplies_[reply.replica_id()] = reply;
    checkpointReplySigs_[reply.replica_id()] = std::string(sig.begin(), sig.end());

    // Don't try finishing the commit if our log hasn't reached the seq being committed
    if (checkpointReplies_[replicaId_].seq() != checkpointSeq_) {
        VLOG(4) << "Skipping processing of commit messages until we receive our own...";
        return;
    }

    if (checkpointCert_.has_value() && checkpointCert_->seq() >= reply.seq()) {
        VLOG(4) << "Checkpoint: already have cert for seq=" << reply.seq() << ", skipping";
        return;
    }
    std::map<std::tuple<std::string, int, int>, std::set<int>> matchingReplies;

    // Find a cert among a set of replies
    for (const auto &entry : checkpointReplies_) {
        uint32_t replicaId = entry.first;
        const Reply &reply = entry.second;

        // Already committed
        if (reply.seq() <= log_->checkpoint.seq)
            continue;

        VLOG(4) << digest_to_hex(reply.digest()).substr(0, 8) << " " << reply.seq() << " " << reply.instance();

        std::tuple<std::string, uint32_t, uint32_t> key = {reply.digest(), reply.instance(), reply.seq()};

        matchingReplies[key].insert(replicaId);

        bool hasOwnReply = checkpointReplies_.count(replicaId_) && checkpointReplies_[replicaId_].seq() == reply.seq();

        // Need 2f + 1 and own reply
        if (matchingReplies[key].size() >= 2 * f_ + 1 && hasOwnReply) {
            checkpointCert_ = Cert();
            checkpointCert_->set_seq(std::get<2>(key));

            for (auto repId : matchingReplies[key]) {
                checkpointCert_->add_signatures(checkpointReplySigs_[repId]);
                (*checkpointCert_->add_replies()) = checkpointReplies_[repId];
            }

            VLOG(1) << "Checkpoint: created cert for request number " << reply.seq();

            const byte *logDigest = log_->getDigest(reply.seq());
            std::string appDigest = log_->app_->getDigest(reply.seq());
            ClientRecords tmpClientRecords = checkpointClientRecords_;
            uint64_t inst = instance_;
            threadpool_.enqueueTask([=, this](byte *buffer) {
                // Broadcast commmit Message
                dombft::proto::Commit commit;
                commit.set_replica_id(replicaId_);
                commit.set_seq(reply.seq());
                commit.set_log_digest((const char *) logDigest, SHA256_DIGEST_LENGTH);
                commit.set_app_digest(appDigest);
                commit.set_instance(inst);

                toProtoClientRecords(commit, tmpClientRecords);
                broadcastToReplicas(commit, MessageType::COMMIT, buffer);
            });

            return;
        }
    }
}

void Replica::handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig)
{
    std::lock_guard<std::mutex> guard(replicaStateMutex_);

    VLOG(3) << "Processing COMMIT from " << commitMsg.replica_id() << " for seq " << commitMsg.seq();

    checkpointCommits_[commitMsg.replica_id()] = commitMsg;
    checkpointCommitSigs_[commitMsg.replica_id()] = std::string(sig.begin(), sig.end());

    if (log_->checkpoint.seq >= commitMsg.seq()) {
        VLOG(4) << "Checkpoint: already committed for seq=" << commitMsg.seq() << ", skipping";
        return;
    }

    std::map<std::tuple<std::string, std::string, int, int>, std::set<int>> matchingCommits;

    // Find a cert among a set of replies
    for (const auto &[replicaId, commit] : checkpointCommits_) {
        // Already committed
        if (commit.seq() <= log_->checkpoint.seq)
            continue;

        std::tuple<std::string, std::string, uint32_t, uint32_t> key = {commit.log_digest(), commit.app_digest(),
                                                                        commit.instance(), commit.seq()};

        matchingCommits[key].insert(replicaId);

        // TODO see if there's a better way to do this
        bool hasOwnCommit =
            checkpointCommits_.count(replicaId_) && checkpointCommits_[replicaId_].seq() == commit.seq();

        // Need 2f + 1 and own commit
        // Modifies log if checkpoint is incosistent with our current log
        if (matchingCommits[key].size() >= 2 * f_ + 1 && hasOwnCommit) {
            uint32_t seq = commit.seq();

            LOG(INFO) << "Committing seq=" << seq;
            VLOG(1) << "PERF event=checkpoint_end seq=" << seq;

            log_->checkpoint.seq = seq;

            memcpy(log_->checkpoint.appDigest, commit.app_digest().c_str(), commit.app_digest().size());
            memcpy(log_->checkpoint.logDigest, commit.log_digest().c_str(), commit.log_digest().size());

            // TODO(Hao) why store these?
            for (uint32_t r : matchingCommits[key]) {
                log_->checkpoint.commitMessages[r] = checkpointCommits_[r];
                log_->checkpoint.signatures[r] = checkpointCommitSigs_[r];
            }
            log_->commit(log_->checkpoint.seq);

            const byte *myDigestBytes = log_->getDigest(seq);
            std::string myDigest(myDigestBytes, myDigestBytes + SHA256_DIGEST_LENGTH);

            if (myDigest != commit.log_digest()) {
                LOG(INFO) << "Local log digest does not match committed digest, overwriting app snapshot and modifying "
                             "log...";
                // TODO: counter uses digest as snapshot, need to generalize this
                log_->app_->applySnapshot(commit.app_digest());
                VLOG(5) << "Apply commit: old_digest=" << digest_to_hex(myDigest).substr(56)
                        << " new_digest=" << digest_to_hex(commit.log_digest()).substr(56);
                checkpointClientRecords_.clear();
                getClientRecordsFromProto(commit.client_records(), checkpointClientRecords_);
                clientRecords_ = checkpointClientRecords_;
                reapplyEntriesWithRecord(log_->checkpoint.seq + 1, 0);
            }else{
                checkpointClientRecords_ = intermediateCheckpointClientRecords_;
            }

            return;
        }
    }
}


void Replica::sendMsgToDst(const google::protobuf::Message &msg, MessageType type, const Address &dst, byte *buf)
{
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buf);
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    endpoint_->SendPreparedMsgTo(dst, hdr);
}

void Replica::broadcastToReplicas(const google::protobuf::Message &msg, MessageType type, byte *buf)
{
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type, buf);
    // TODO check errors for all of these lol
    // TODO this sends to self as well, could shortcut this
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr, hdr);
    }
}

bool Replica::verifyCert(const Cert &cert)
{
    LOG(INFO) << "Verify cert triggered";
    if (cert.replies().size() < 2 * f_ + 1) {
        LOG(INFO) << "Received cert of size " << cert.replies().size() << ", which is smaller than 2f + 1, f=" << f_;
        return false;
    }

    if (cert.replies().size() != cert.signatures().size()) {
        LOG(INFO) << "Cert replies size " << cert.replies().size() << " is not equal to "
                  << "cert signatures size" << cert.signatures().size();
        return false;
    }

    // TODO check cert instance

    // Verify each signature in the cert
    for (int i = 0; i < cert.replies().size(); i++) {
        const Reply &reply = cert.replies()[i];
        const std::string &sig = cert.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();

        if (!sigProvider_.verify((byte *) serializedReply.c_str(), serializedReply.size(), (byte *) sig.c_str(),
                                 sig.size(), "replica", reply.replica_id())) {
            LOG(INFO) << "Cert failed to verify!";
            return false;
        }
    }

    // TOOD verify that cert actually contains matching replies...
    // And that there aren't signatures from the same replica.

    return true;
}

void Replica::startFallback()
{
    std::lock_guard<std::mutex> guard(replicaStateMutex_);

    fallback_ = true;
    instance_++;
    LOG(INFO) << "Starting fallback for instance " << instance_;
    if (endpoint_->isTimerRegistered(fallbackTimer_.get())) {
        endpoint_->ResetTimer(fallbackTimer_.get());
    } else {
        endpoint_->RegisterTimer(fallbackTimer_.get());
    }

    VLOG(1) << "PERF event=fallback_start replica_id=" << replicaId_ << " seq=" << log_->nextSeq
            << " instance=" << instance_;

    // Extract log into start fallback message
    FallbackStart fallbackStartMsg;
    fallbackStartMsg.set_instance(instance_);
    fallbackStartMsg.set_replica_id(replicaId_);
    log_->toProto(fallbackStartMsg);

    toProtoClientRecords(fallbackStartMsg, checkpointClientRecords_);

    LOG(INFO) << "Sending FALLBACK_START to replica " << instance_ % replicaAddrs_.size();
    sendMsgToDst(fallbackStartMsg, FALLBACK_START, replicaAddrs_[instance_ % replicaAddrs_.size()]);
    LOG(INFO) << "DUMP start fallback instance=" << instance_ << " " << *log_;
}

void Replica::handleFallbackStart(const FallbackStart &msg, std::span<byte> sig)
{
    fallbackHistory_[msg.replica_id()] = msg;
    fallbackHistorySigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received fallback message from " << msg.replica_id();

    if (!isPrimary()) {
        return;
    }

    // First check if we have 2f + 1 fallback start messages for the same instance
    auto numStartMsgs = std::count_if(fallbackHistory_.begin(), fallbackHistory_.end(),
                                      [&](auto &startMsg) { return startMsg.second.instance() == msg.instance(); });

    if (numStartMsgs == 2 * f_ + 1) {
        doPrePreparePhase();
    }
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
    std::lock_guard<std::mutex> guard(replicaStateMutex_);

    if (!fallbackProposal_.has_value()) {
        LOG(ERROR) << "Attempted to finishFallback without a proposal!";
        return;
    }
    FallbackProposal &history = fallbackProposal_.value();
    LOG(INFO) << "Applying fallback for instance=" << history.instance() << " from instance=" << instance_;
    instance_ = history.instance();
    endpoint_->UnRegisterTimer(fallbackTimer_.get());

    LogSuffix logSuffix;
    getLogSuffixFromProposal(history, logSuffix);

    logSuffix.replicaId = replicaId_;
    logSuffix.instance = instance_;

    applySuffixToLog(logSuffix, log_);
    clientRecords_ = logSuffix.clientRecords;

    VLOG(1) << "PERF event=fallback_end replica_id=" << replicaId_ << " seq=" << log_->nextSeq
            << " instance=" << instance_;
    LOG(INFO) << "DUMP finish fallback instance=" << instance_ << " " << *log_;

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
    fallbackProposal_.reset();

    // Start checkpoint for last spot in the log, which should finish if fallback was sucessful
    // TODO, we could also directly just set the checkpoint here.

    checkpointSeq_ = log_->nextSeq - 1;
    intermediateCheckpointClientRecords_ = clientRecords_;
    LOG(INFO) << "Starting checkpoint cert for seq=" << checkpointSeq_;

    checkpointCert_.reset();
    Reply reply;
    replyFromLogEntry(reply, checkpointSeq_);
    broadcastToReplicas(reply, MessageType::REPLY);
}

// dummy fallbacl PBFT

void Replica::doPrePreparePhase()
{
    std::lock_guard<std::mutex> guard(replicaStateMutex_);

    if (!isPrimary()) {
        LOG(ERROR) << "Attempted to doPrePrepare from non-primary replica!";
        return;
    }
    VLOG(6) << "DUMMY PrePrepare for instance=" << instance_ << " in primary replicaId=" << replicaId_;

    FallbackPrePrepare prePrepare;
    prePrepare.set_primary_id(replicaId_);
    prePrepare.set_instance(instance_);

    // Piggyback the fallback proposal
    FallbackProposal *proposal = prePrepare.mutable_proposal();
    proposal->set_replica_id(replicaId_);
    proposal->set_instance(instance_);
    for (auto &startMsg : fallbackHistory_) {
        if (startMsg.second.instance() != instance_)
            continue;

        *(proposal->add_logs()) = startMsg.second;
    }
    broadcastToReplicas(prePrepare, DUMMY_PREPREPARE);
}

void Replica::doPreparePhase()
{
    VLOG(6) << "DUMMY Prepare for instance=" << instance_ << " replicaId=" << replicaId_;
    FallbackPrepare prepare;
    prepare.set_replica_id(replicaId_);
    prepare.set_instance(instance_);
    broadcastToReplicas(prepare, DUMMY_PREPARE);
}

void Replica::doCommitPhase()
{
    VLOG(6) << "DUMMY Commit for instance=" << instance_ << " replicaId=" << replicaId_;
    FallbackPBFTCommit cmt;
    cmt.set_replica_id(replicaId_);
    cmt.set_instance(instance_);
    broadcastToReplicas(cmt, DUMMY_COMMIT);
}
void Replica::handlePrePrepare(const FallbackPrePrepare &msg)
{
    VLOG(6) << "DUMMY PrePrepare RECEIVED for instance=" << msg.instance() << " from replicaId=" << msg.primary_id();
    // TODO Verify FallbackProposal
    if (fallbackProposal_.has_value()) {
        LOG(ERROR) << "Attempted to doPrepare with existing fallbackProposal!";
        return;
    }
    fallbackProposal_ = msg.proposal();
    doPreparePhase();
}

void Replica::handlePrepare(const FallbackPrepare &msg)
{
    VLOG(6) << "DUMMY Prepare RECEIVED for instance=" << msg.instance() << " from replicaId=" << msg.replica_id();
    fallbackPrepares_[msg.replica_id()] = msg;
    auto numMsgs = std::count_if(fallbackPrepares_.begin(), fallbackPrepares_.end(),
                                 [this](auto &curMsg) { return curMsg.second.instance() == instance_; });
    if (numMsgs == 2 * f_ + 1) {
        VLOG(6) << "DUMMY Prepare received from 2f + 1 replicas, Prepared!";
        doCommitPhase();
    }
}

void Replica::handlePBFTCommit(const FallbackPBFTCommit &msg)
{
    VLOG(6) << "DUMMY Commit RECEIVED for instance=" << msg.instance() << " from replicaId=" << msg.replica_id();
    fallbackPBFTCommits_[msg.replica_id()] = msg;
    auto numMsgs = std::count_if(fallbackPBFTCommits_.begin(), fallbackPBFTCommits_.end(),
                                 [this](auto &curMsg) { return curMsg.second.instance() == instance_; });
    if (numMsgs == 2 * f_ + 1) {
        VLOG(6) << "DUMMY Commit received from 2f + 1 replicas, Committed!";
        finishFallback();
    }
}


bool Replica::checkAndUpdateClientRecord(const ClientRequest &clientHeader){
    uint32_t clientId = clientHeader.client_id();
    uint32_t clientSeq = clientHeader.client_seq();
    uint32_t clientInstance = clientHeader.instance();

    std::lock_guard<std::mutex> guard(replicaStateMutex_);
    ClientRecord &cliRecord = clientRecords_[clientId];
    cliRecord.instance_ = std::max(clientInstance, cliRecord.instance_);

    if (clientInstance < instance_) {
        LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                  << " due to stale instance! Sending blank reply to catch client up";
        if (cliRecord.instance_ < instance_) {
            // Send blank request to catch up the client
            // update this so we don't send this multiple times
            cliRecord.instance_ = instance_;
            threadpool_.enqueueTask([this, clientId, inst = instance_](byte *buffer) {
                // Send blank request to catch up the client
                Reply reply;
                reply.set_replica_id(replicaId_);
                reply.set_client_id(clientId);
                reply.set_instance(inst);

                sendMsgToDst(reply, MessageType::REPLY, clientAddrs_[clientId], buffer);
                return;
            });
        }
        return false;
    }

    if (!updateRecordWithSeq(cliRecord, clientSeq)) {
        LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                  << " due to duplication!";
        return false;
    }

    return true;

}

void Replica::reapplyEntriesWithRecord(uint32_t startingSeq, uint32_t rShiftNum)
{
    log_->app_->abort(startingSeq - 1);
    uint32_t curSeq = startingSeq;
    for (uint32_t s = startingSeq - rShiftNum; s < log_->nextSeq; s++) {
        std::shared_ptr<::LogEntry> entry = log_->getEntry(s);

        // record update
        uint32_t clientId = entry->client_id;
        uint32_t clientSeq = entry->client_seq;
        ClientRecord& cliRecord = clientRecords_[clientId];
        cliRecord.instance_ = instance_;
        if(!updateRecordWithSeq(cliRecord, clientSeq)) {
            LOG(INFO) << "Dropping request c_id=" << clientId << " c_seq=" << clientSeq
                      << " due to duplication in reapplying with record!";
            continue;
        }

        log_->app_->execute(entry->request, curSeq);

        entry->updateDigest(curSeq == log_->checkpoint.seq + 1 ? log_->checkpoint.logDigest
                                         : log_->getEntry(curSeq-1)->digest);

        VLOG(5) << "PERF event=update_digest seq=" << curSeq
                << " digest=" << digest_to_hex(entry->digest).substr(56);
        curSeq++;
    }

    if(curSeq != log_->nextSeq)
        log_->nextSeq = curSeq;
}



}   // namespace dombft