#include "replica.h"

#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include "lib/application.h"

#include "lib/apps/counter.h"

#include <assert.h>
#include <openssl/pem.h>
#include <sstream>

namespace dombft {
using namespace dombft::proto;

Replica::Replica(const ProcessConfig &config, uint32_t replicaId, uint32_t triggerFallbackFreq)
    : replicaId_(replicaId), instance_(0), triggerFallbackFreq_(triggerFallbackFreq)
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

    f_ = config.replicaIps.size() / 3;

    LOG(INFO) << "instantiating log";
    log_ = std::make_shared<Log>(config.app);
    LOG(INFO) << "log instantiated";

    if (config.transport == "nng") {
        auto addrPairs = getReplicaAddrs(config, replicaId_);
        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, true);

        size_t nClients = config.clientIps.size();
        for (size_t i = 0; i < nClients; i++) {
            // LOG(INFO) << "Client " << i << ": " << addrPairs[i].second.ip();
            clientAddrs_.push_back(addrPairs[i].second);
        }

        for (size_t i = nClients + 1; i < addrPairs.size(); i++) {
            // Skip adding one side of self connection so replica chooses one side to send to
            // TODO this is very ugly.
            if (i - (nClients + 1) == replicaId_)
                continue;
            replicaAddrs_.push_back(addrPairs[i].second);
        }
    } else {
        endpoint_ = std::make_unique<UDPEndpoint>(bindAddress, replicaPort, true);

        /** Store all replica addrs */
        for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
            replicaAddrs_.push_back(Address(config.replicaIps[i], config.replicaPort));
        }

        /** Store all client addrs */
        for (uint32_t i = 0; i < config.clientIps.size(); i++) {
            clientAddrs_.push_back(Address(config.clientIps[i], config.clientPort));
        }
    }

    MessageHandlerFunc handler = [this](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        this->handleMessage(msgHdr, msgBuffer, sender);
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

#if PROTOCOL == DOMBFT
void Replica::handleMessage(MessageHeader *hdr, byte *body, Address *sender)
{
    if (hdr->msgLen < 0) {
        return;
    }

#if USE_PROXY

#if FABRIC_CRYPTO
    // TODO find id correctly
    if (!sigProvider_.verify(hdr, body, "receiver", 0)) {
        LOG(INFO) << "Failed to verify receiver signatures";
        return;
    }
#endif

    if (hdr->msgType == MessageType::DOM_REQUEST) {
        DOMRequest domHeader;
        ClientRequest clientHeader;

        if (!domHeader.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse DOM_REQUEST message";
            return;
        }

        // TODO This seems bad...
        // Separate this out into another function probably.
        MessageHeader *clientMsgHdr = (MessageHeader *) domHeader.client_req().c_str();
        byte *clientBody = (byte *) (clientMsgHdr + 1);
        if (!clientHeader.ParseFromArray(clientBody, clientMsgHdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            return;
        }

        if (!sigProvider_.verify(clientMsgHdr, clientBody, "client", clientHeader.client_id())) {
            LOG(INFO) << "Failed to verify client signature!";
            return;
        }

        // TODO TODO get rid of this
        if (fallback_) {
            LOG(INFO) << "Queuing request due to fallback";
            fallbackQueuedReqs_.push_back({domHeader.deadline(), clientHeader});
            return;
        }

        if(triggerFallbackFreq_ && replicaId_ % 2 && log_->nextSeq % triggerFallbackFreq_ == 0)
            holdAndSwapCliReq(clientHeader);
        else
            handleClientRequest(clientHeader);
    }
#else
    if (hdr->msgType == CLIENT_REQUEST) {
        ClientRequest clientHeader;

        if (!clientHeader.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
            return;
        }

        if (!sigProvider_.verify(hdr, body, "client", clientHeader.client_id())) {
            LOG(INFO) << "Failed to verify client signature!";
            return;
        }

        handleClientRequest(clientHeader);
    }
#endif

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

        if (!sigProvider_.verify(hdr, body, "replica", replyHeader.replica_id())) {
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

        if (!sigProvider_.verify(hdr, body, "replica", commitMsg.replica_id())) {
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

        if (!sigProvider_.verify(hdr, body, "client", fallbackTriggerMsg.client_id())) {
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

        if (!sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        // TODO handle case where instance is higher
        handleFallbackStart(msg, std::span{body + hdr->msgLen, hdr->sigLen});
    }

    if (hdr->msgType == FALLBACK_PROPOSAL) {
        FallbackProposal msg;

        if (!msg.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse FALLBACK_PROPOSAL message";
            return;
        }

        if (!sigProvider_.verify(hdr, body, "replica", msg.replica_id())) {
            LOG(INFO) << "Failed to verify replica signature!";
            return;
        }

        if (msg.instance() < instance_) {

            LOG(INFO) << "Received old fallback proposal from instance=" << msg.instance() << " own instance is "
                      << instance_;
            return;
        }

        // TODO actually run a protocol instead of assuming this is ok
        finishFallback(msg);
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
    Reply reply;
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();
    VLOG(2) << "Received request from client " << clientId << " with seq " << clientSeq;

    if (clientId < 0 || clientId > clientAddrs_.size()) {
        LOG(ERROR) << "Invalid client id" << clientId;
        return;
    }
    reply.set_client_id(clientId);
    reply.set_client_seq(clientSeq);
    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_);

    std::string result;

    bool success = log_->addEntry(clientId, clientSeq, request.req_data(), result);

    if (!success) {
        // TODO Handle this more gracefully by queuing requests
        LOG(ERROR) << "Could not add request to log!";
        return;
    }
    uint32_t seq = log_->nextSeq - 1;
    reply.set_result(result);
    reply.set_fast(true);
    reply.set_seq(seq);
    reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);

    // VLOG(4) << "Start prepare message";
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
    // VLOG(4) << "Finish Serialization, start signature";

    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    // VLOG(4) << "Finish signature";

    LOG(INFO) << "Sending reply back to client " << clientId;
    endpoint_->SendPreparedMsgTo(clientAddrs_[clientId]);

    LOG(INFO) << "Done Sending reply back to client " << clientId;

    // Try and commit every 10 replies (half of the way before
    // we can't speculatively execute anymore)
    if (seq % CHECKPOINT_INTERVAL == 0) {
        LOG(INFO) << "Starting checkpoint cert for seq=" << seq;

        checkpointSeq_ = seq;
        checkpointCert_.reset();

        // TODO remove execution result here
        broadcastToReplicas(reply, MessageType::REPLY);
    }
}


void Replica::messReplyDigest(Reply &reply){
    VLOG(2) << "Digest altered for replica: " << replicaId_ << " seq: " << reply.seq();
    byte tmpDigest[SHA256_DIGEST_LENGTH];
    memcpy(tmpDigest, log_->getDigest(), SHA256_DIGEST_LENGTH);
    tmpDigest[0] += replicaId_;
    reply.set_digest(tmpDigest, SHA256_DIGEST_LENGTH);
}

std::optional<uint32_t> Replica::swapLog(uint32_t seqA, uint32_t seqB) {
    if(seqA == seqB || seqA < log_->checkpoint.seq || seqB < log_->checkpoint.seq || seqA >= log_->nextSeq || seqB >= log_->nextSeq) {
        return {};
    }
    if (seqA > seqB) {
        std::swap(seqA, seqB);
    }
    VLOG(2) << "Swapping log for seq "<<seqA<<" and "<<seqB<<" for replica: " << replicaId_;
    uint32_t aIdx = seqA % MAX_SPEC_HIST;
    uint32_t bIdx = seqB % MAX_SPEC_HIST;
    std::swap(log_->log[aIdx], log_->log[bIdx]);

    // update all digest from seqA to the latest log
    uint32_t curIdx = aIdx;
    uint32_t lastIdx = log_->nextSeq % MAX_SPEC_HIST;
    uint32_t curSeq = seqA;
    while(curIdx!=MAX_SPEC_HIST && curIdx != lastIdx){
        log_->modifyEntry(curSeq++, *(log_->log[curIdx++]));
    }
    if( curIdx != lastIdx){
        curIdx = 0;
        while(curIdx != bIdx){
            log_->modifyEntry(curSeq++, *(log_->log[curIdx++]));
        }
    }
    return seqA;
}

void Replica::holdAndSwapCliReq(const proto::ClientRequest &request) {
    uint32_t clientId = request.client_id();
    uint32_t clientSeq = request.client_seq();
    if(!heldRequest_){
        heldRequest_ = request;
        VLOG(2) << "Holding request (" << clientId << ", " << clientSeq << ") for swapping";
        return;
    }
    handleClientRequest(request);
    handleClientRequest(heldRequest_.value());
    VLOG(2) << "Swapped requests (" << clientId << ", " << clientSeq << ") and ("
            << heldRequest_->client_id() << ", " << heldRequest_->client_seq() << ")";
    heldRequest_.reset();
}

void Replica::handleCert(const Cert &cert)
{
    if (!verifyCert(cert)) {
        return;
    }

    const Reply &r = cert.replies()[0];
    log_->addCert(r.seq(), cert);

    CertReply reply;
    reply.set_client_id(r.client_id());
    reply.set_client_seq(r.client_seq());
    reply.set_replica_id(replicaId_);

    VLOG(3) << "Sending cert ack for " << reply.client_id() << ", " << reply.client_seq() << " to "
            << clientAddrs_[reply.client_id()].ip();

    // TODO set result
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::CERT_REPLY);
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    endpoint_->SendPreparedMsgTo(clientAddrs_[reply.client_id()]);
}

void Replica::handleReply(const dombft::proto::Reply &reply, std::span<byte> sig)
{
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
        int replicaId = entry.first;
        const Reply &reply = entry.second;

        VLOG(4) << digest_to_hex(reply.digest()).substr(56) << " " << reply.seq() << " " << reply.instance();

        // Already committed
        if (reply.seq() <= log_->checkpoint.seq)
            continue;

        std::tuple<std::string, int, int> key = {reply.digest(), reply.instance(), reply.seq()};

        matchingReplies[key].insert(replicaId);

        // Need 2f + 1 and own reply
        if (matchingReplies[key].size() >= 2 * f_ + 1 && checkpointReplies_.count(replicaId_)) {

            checkpointCert_ = Cert();
            checkpointCert_->set_seq(std::get<2>(key));

            for (auto repId : matchingReplies[key]) {
                checkpointCert_->add_signatures(checkpointReplySigs_[repId]);
                (*checkpointCert_->add_replies()) = checkpointReplies_[repId];
            }

            VLOG(1) << "Checkpoint: created cert for request number " << reply.seq();

            const byte *logDigest = log_->getDigest(reply.seq());

            // Broadcast commmit Message
            // TODO get app digest
            dombft::proto::Commit commit;
            commit.set_replica_id(replicaId_);
            commit.set_seq(reply.seq());
            commit.set_log_digest((const char *) logDigest, SHA256_DIGEST_LENGTH);
            commit.set_app_digest(log_->app_->getDigest(reply.seq()));

            broadcastToReplicas(commit, MessageType::COMMIT);
            return;
        }
    }
}

void Replica::handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig)
{
    VLOG(3) << "Processing COMMIT from " << commitMsg.replica_id() << " for seq " << commitMsg.seq();

    checkpointCommits_[commitMsg.replica_id()] = commitMsg;
    checkpointCommitSigs_[commitMsg.replica_id()] = std::string(sig.begin(), sig.end());

    // Don't try finishing the commit if our log hasn't reached the seq being committed
    if (checkpointCommits_[replicaId_].seq() != checkpointSeq_) {
        VLOG(4) << "Skipping processing of commit messages until we receive our own...";
        return;
    }

    if (log_->checkpoint.seq >= commitMsg.seq()) {
        VLOG(4) << "Checkpoint: already committed for seq=" << commitMsg.seq() << ", skipping";
        return;
    }

    // DO something simpler than client, just match everything to my commit
    // If this doesn't work, we won't be able to commit without fixing anyways!
    const dombft::proto::Commit &myCommit = checkpointCommits_[replicaId_];
    std::vector<uint32_t> matches = {replicaId_};

    for (auto &[r, commit] : checkpointCommits_) {
        if (r == replicaId_) {
            // Skip processing own checkpoint, we assume it is good
            continue;
        }

        if (commit.seq() != myCommit.seq()) {
            continue;
        }

        if (commit.log_digest() != myCommit.log_digest() || commit.app_digest() != myCommit.app_digest()) {
            VLOG(4) << "Found non matching digest!";
            continue;
        }

        matches.push_back(r);
    }

    if (matches.size() >= 2 * f_ + 1) {
        LOG(INFO) << "Committing seq=" << myCommit.seq();

        log_->checkpoint.seq = myCommit.seq();
        memcpy(log_->checkpoint.appDigest, myCommit.app_digest().c_str(), SHA256_DIGEST_LENGTH);
        memcpy(log_->checkpoint.logDigest, myCommit.log_digest().c_str(), SHA256_DIGEST_LENGTH);

        for (uint32_t r : matches) {
            log_->checkpoint.commitMessages[r] = checkpointCommits_[r];
            log_->checkpoint.signatures[r] = checkpointCommitSigs_[r];
        }
        log_->commit(log_->checkpoint.seq);
    }
}

void Replica::broadcastToReplicas(const google::protobuf::Message &msg, MessageType type)
{
    MessageHeader *hdr = endpoint_->PrepareProtoMsg(msg, type);
    // TODO check errors for all of these lol
    // TODO this sends to self as well, could shortcut this
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    for (const Address &addr : replicaAddrs_) {
        endpoint_->SendPreparedMsgTo(addr);
    }
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

    // Verify each signature in the cert
    for (int i = 0; i < cert.replies().size(); i++) {
        const Reply &reply = cert.replies()[i];
        const std::string &sig = cert.signatures()[i];
        std::string serializedReply = reply.SerializeAsString();   // TODO skip reseraizliation here?

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
    fallback_ = true;
    instance_++;
    LOG(INFO) << "Starting fallback for instance " << instance_;

    // TODO prevent processing of other messsages.

    if (endpoint_->isTimerRegistered(fallbackTimer_.get())) {
        endpoint_->ResetTimer(fallbackTimer_.get());
    } else {
        endpoint_->RegisterTimer(fallbackTimer_.get());
    }

    // Extract log into start fallback message
    FallbackStart fallbackStartMsg;
    fallbackStartMsg.set_instance(instance_);
    fallbackStartMsg.set_replica_id(replicaId_);
    log_->toProto(fallbackStartMsg);

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(fallbackStartMsg, FALLBACK_START);
    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);

    LOG(INFO) << "Sending FALLBACK_START to replica " << instance_ % replicaAddrs_.size();
    endpoint_->SendPreparedMsgTo(replicaAddrs_[instance_ % replicaAddrs_.size()]);

    // TEMP: dump the logs
    LOG(INFO) << "DUMP start fallback instance=" << instance_ << " " << *log_;
}

void Replica::handleFallbackStart(const FallbackStart &msg, std::span<byte> sig)
{
    fallbackHistory_[msg.replica_id()] = msg;
    fallbackHistorySigs_[msg.replica_id()] = std::string(sig.begin(), sig.end());

    LOG(INFO) << "Received fallback message from " << msg.replica_id();

    if (instance_ % replicaAddrs_.size() != replicaId_) {
        return;
    }

    // First check if we have 2f + 1 fallback start messages for the same instance
    auto numStartMsgs = std::count_if(fallbackHistory_.begin(), fallbackHistory_.end(),
                                      [this](auto &startMsg) { return startMsg.second.instance() == instance_; });

    if (numStartMsgs == 2 * f_ + 1) {
        FallbackProposal proposal;

        proposal.set_replica_id(replicaId_);
        proposal.set_instance(instance_);
        for (auto &startMsg : fallbackHistory_) {
            if (startMsg.second.instance() != instance_)
                continue;

            *(proposal.add_logs()) = startMsg.second;
        }

        broadcastToReplicas(proposal, FALLBACK_PROPOSAL);
    }
}

void Replica::applyFallbackReq(const dombft::proto::LogEntry &entry)
{
    uint32_t clientId = entry.client_id();

    VLOG(2) << log_->nextSeq << ": c_id=" << clientId << " c_seq=" << entry.client_seq();

    Reply reply;
    std::string result;

    if (!log_->addEntry(clientId, entry.client_seq(), entry.request(), result)) {
        LOG(ERROR) << "Failure to add logg entry!";
    }
    // TODO add an interface here for adding executed requests!

    reply.set_client_id(clientId);
    reply.set_client_seq(entry.client_seq());
    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_ - 1);   // Fallback is wrapping up the previous instance
    reply.set_result(result);

    uint32_t seq = log_->nextSeq - 1;
    // TODO actually get the result here.
    reply.set_fast(true);
    reply.set_seq(seq);

    reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);

    MessageHeader *hdr = endpoint_->PrepareProtoMsg(reply, MessageType::REPLY);
    // VLOG(4) << "Finish Serialization, start signature";

    sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
    // VLOG(4) << "Finish signature";

    LOG(INFO) << "Sending reply back to client " << clientId;
    endpoint_->SendPreparedMsgTo(clientAddrs_[clientId]);
}

void Replica::finishFallback(const FallbackProposal &history)
{
    LOG(INFO) << "Applying fallback for instance=" << history.instance() << " from instance=" << instance_;
    fallback_ = false;
    instance_ = history.instance();

    // TODO verify message so this isn't unsafe
    uint32_t maxCheckpointSeq = 0;
    uint32_t maxCheckpointIdx = 0;

    // First find highest commit point
    for (int i = 0; i < history.logs().size(); i++) {
        auto &log = history.logs()[i];
        if (log.checkpoint().seq() > maxCheckpointSeq) {
            maxCheckpointSeq = log.checkpoint().seq();
            maxCheckpointIdx = i;
        }
    }

    VLOG(4) << "Highest checkpoint is for seq=" << maxCheckpointSeq;

    // Find highest request with a cert
    // By quorum intersection there can't be conflicting certificates, unless one is
    // from an old instance (TODO handle this)

    uint32_t maxSeq = 0;

    // Idx of log we will use to match our logs to the fallback agreed upon logs.
    uint32_t logToUseIdx = 0;
    uint32_t logToUseSeq = 0;

    const Cert *maxSeqCert = nullptr;

    for (int i = 0; i < history.logs().size(); i++) {
        auto &log = history.logs()[i];
        // TODO verify each checkpoint

        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            if (!entry.has_cert())
                continue;
            // Already included in checkpoint
            if (entry.seq() <= maxCheckpointSeq)
                continue;

            // TODO verify cert
            if (entry.seq() > maxSeq) {
                VLOG(4) << "Cert found for seq=" << entry.seq() << " c_id=" << entry.cert().replies()[0].client_id()
                        << " c_seq=" << entry.cert().replies()[0].client_seq();

                maxSeqCert = &entry.cert();
                logToUseIdx = i;
                logToUseSeq = entry.seq();
            }
        }
    }

    // TODO verify first
    // TODO add some cert util methods
    // std::string certDigest = maxSeqCert->replies()[0].digest();
    uint32_t certSeq = maxSeq;

    // Counts of matching digests for each seq coming after max cert
    std::map<uint32_t, std::map<std::string, int>> matchingEntries;
    std::map<uint32_t, uint32_t> maxMatchSeq;
    std::map<uint32_t, uint32_t> maxAppliedSeq;

    // TODO save this info in the checkpoint
    std::map<uint32_t, std::map<uint32_t, const LogEntry &>> clientReqs;

    for (int i = 0; i < history.logs().size(); i++) {
        auto &log = history.logs()[i];
        // TODO verify each checkpoint

        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            if (entry.seq() <= certSeq)
                continue;

            matchingEntries[entry.seq()][entry.digest()]++;
            clientReqs[entry.client_id()].emplace(entry.client_seq(), entry);

            if (matchingEntries[entry.seq()][entry.digest()] == f_ + 1) {
                VLOG(4) << "f + 1 matching digests found for seq=" << entry.seq() << " c_id=" << entry.client_id()
                        << " c_seq=" << entry.client_seq();

                maxMatchSeq[entry.client_id()] = std::max(maxMatchSeq[entry.client_id()], entry.client_seq());
                maxAppliedSeq[entry.client_id()] = maxMatchSeq[entry.client_id()];
                logToUseIdx = i;
                logToUseSeq = entry.seq();
            }
        }
    }

    LOG(INFO) << "Applying checkpoint";

    const LogCheckpoint &maxCheckpoint = history.logs()[maxCheckpointIdx].checkpoint();

    // TODO this only works with our basic counter because app_digest == counter!
    log_->app_->applySnapshot(maxCheckpoint.app_digest());

    log_->checkpoint.seq = maxCheckpoint.seq();
    memcpy(log_->checkpoint.appDigest, maxCheckpoint.app_digest().c_str(), maxCheckpoint.app_digest().size());
    memcpy(log_->checkpoint.logDigest, maxCheckpoint.log_digest().c_str(), maxCheckpoint.log_digest().size());
    log_->checkpoint.cert = maxCheckpoint.cert();

    for (uint32_t i = 0; i < maxCheckpoint.commits().size(); i++) {
        auto &commit = maxCheckpoint.commits()[i];

        log_->checkpoint.commitMessages[commit.replica_id()] = commit;
        log_->checkpoint.signatures[commit.replica_id()] = maxCheckpoint.signatures()[i];
    }

    LOG(INFO) << "Checkpoint digest=" << digest_to_hex(log_->checkpoint.logDigest).substr(56);

    LOG(INFO) << "Applying requests up to cert and with f + 1 requests";

    // TODO rollback
    bool rollbackDone = false;

    auto &logToUse = history.logs()[logToUseIdx].log_entries();
    for (const dombft::proto::LogEntry &entry : logToUse) {
        if (entry.seq() <= maxCheckpointSeq)
            continue;
        if (entry.seq() > logToUseSeq)
            break;
        // TODO skip ones already in our log.
        // TODO access entry needs to be cleaner than this
        // TODO fix namespaces lol
        std::shared_ptr<::LogEntry> myEntry = log_->log[entry.seq() % MAX_SPEC_HIST];
        if (entry.seq() < log_->nextSeq && myEntry->seq != entry.seq()) {
            LOG(ERROR) << "attempt to access log seq not in my log! " << myEntry->seq << " " << entry.seq();
            exit(1);
        }

        std::string myDigest(myEntry->digest, myEntry->digest + SHA256_DIGEST_LENGTH);
        if (myDigest == entry.digest()) {
            VLOG(2) << "Skipping c_id=" << entry.client_id() << " c_seq=" << entry.client_seq()
                    << " since already in log at seq=" << entry.seq();
            continue;
        }

        // TODO Rollback application state here!
        if (!rollbackDone) {
            log_->nextSeq = entry.seq();
            log_->app_->abort(entry.seq() - 1);

            rollbackDone = true;
        }

        applyFallbackReq(entry);
    }

    // TODO Rollback application state here!
    if (!rollbackDone) {
        log_->nextSeq = logToUseSeq + 1;
    }

    LOG(INFO) << "Applying rest of requests in histories";

    for (auto &[c_id, reqs] : clientReqs) {
        for (auto &[c_seq, entry] : reqs) {
            // Already matched
            if (c_seq <= maxAppliedSeq[c_id])
                continue;
            maxAppliedSeq[c_id] = c_seq;
            applyFallbackReq(entry);
        }
    }

    // Start checkpoint here
    uint32_t seq = log_->nextSeq - 1;

    LOG(INFO) << "Starting checkpoint cert for seq=" << seq;
    Reply reply;
    ::LogEntry *entry = log_->getEntry(seq);   // TODO better namespace
    reply.set_client_id(entry->client_id);
    reply.set_client_seq(entry->client_seq);
    reply.set_replica_id(replicaId_);
    reply.set_instance(instance_);
    reply.set_result(entry->result);
    reply.set_seq(seq);
    reply.set_digest(log_->getDigest(), SHA256_DIGEST_LENGTH);

    checkpointSeq_ = log_->nextSeq - 1;
    checkpointCert_.reset();
    broadcastToReplicas(reply, MessageType::REPLY);   // TODO better namespace

    // Add rest of the requests in.

    LOG(INFO) << "Finishing fallback by applying the rest of the queued requests!";
    fallback_ = false;
    endpoint_->UnRegisterTimer(fallbackTimer_.get());

    sort(fallbackQueuedReqs_.begin(), fallbackQueuedReqs_.end(), [](auto &l, auto &r) { return l.first < r.first; });

    for (auto &[_, request] : fallbackQueuedReqs_) {
        if (request.client_seq() <= maxAppliedSeq[request.client_id()]) {
            VLOG(2) << "Skipping c_id=" << request.client_id() << " c_seq=" << request.client_seq()
                    << " since already applied";
            continue;
        }

        handleClientRequest(request);
    }

    fallbackQueuedReqs_.clear();

    // TEMP: dump the logs
    LOG(INFO) << "DUMP finish fallback instance=" << instance_ << " " << *log_;
}

}   // namespace dombft