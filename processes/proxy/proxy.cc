#include "proxy.h"

#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"

namespace dombft {
using namespace dombft::proto;

Proxy::Proxy(const ProcessConfig &config, uint32_t proxyId)
{
    lastDeadline_ = GetMicrosecondTimestamp();
    maxOWD_ = config.proxyMaxOwd;
    latencyBound_ = config.proxyMaxOwd;   // Initialize to max to be more conservative
    proxyId_ = proxyId;
    offsetCoefficient_ = config.proxyOffsetCoefficient;
    LOG(INFO) << "offsetCoefficient=" << config.proxyOffsetCoefficient;

    std::string proxyKey = config.proxyKeysDir + "/proxy" + std::to_string(proxyId) + ".der";
    LOG(INFO) << "Loading key from " << proxyKey;

    if (!sigProvider_.loadPrivateKey(proxyKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    numReceivers_ = config.replicaIps.size();

    proxyBatchEnabled_ = config.proxyBatchEnabled;
    proxyBatchMaxCount_ = config.proxyBatchMaxCount;
    proxyBatchMaxDelay_ = config.proxyBatchMaxDelay;
    if (proxyBatchEnabled_) {
        domReqBatchBuffer_.reserve(proxyBatchMaxCount_);
    }

    if (config.transport == "nng") {
        auto addrPairs = getProxyAddrs(config, proxyId);

        endpoint_ = std::make_unique<NngEndpointThreaded>(addrPairs, false);

        // First nClients addresses are for client connections, rest are for replica connections
        size_t nClients = config.clientIps.size();
        for (size_t i = nClients; i < addrPairs.size(); i++) {
            receiverAddrs_.push_back(addrPairs[i].second);
        }

    } else if (config.transport == "simple-rpc") {
        for (int i = 0; i < numReceivers_; i++) {
            std::string receiverIp = config.replicaIps[i];
            receiverAddrs_.push_back(Address(receiverIp, config.replicaPort));
        }

        endpoint_ =
            std::make_unique<OOORPCEndpoint>(config.proxyIps[proxyId], config.proxyForwardPort, receiverAddrs_, 2);
    } else {

        endpoint_ = std::make_unique<UDPEndpoint>(config.proxyIps[proxyId], config.proxyForwardPort, false);

        for (int i = 0; i < numReceivers_; i++) {
            std::string receiverIp = config.replicaIps[i];
            receiverAddrs_.push_back(Address(receiverIp, config.replicaPort));
        }
    }
}

void Proxy::Terminate() { LOG(INFO) << "Terminating..."; }

Proxy::~Proxy()
{

    // TODO Cleanup more
}

void Proxy::SetDOMRequest(const ClientRequest &inReq, DOMRequest &outReq, MessageHeader *hdr)
{
    uint64_t now = GetMicrosecondTimestamp();
    uint64_t deadline = now + latencyBound_;

    deadline = std::max(deadline, lastDeadline_ + 1);
    lastDeadline_ = deadline;

    outReq.set_send_time(now);
    outReq.set_deadline(deadline);
    outReq.set_proxy_id(proxyId_);

    // TODO set these properly
    outReq.set_deadline_set_size(numReceivers_);
    outReq.set_late(false);

    outReq.set_client_id(inReq.client_id());
    outReq.set_client_seq(inReq.client_seq());
    outReq.set_client_req(hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen);
}

void Proxy::Run()
{
    OWDCalc::PercentileCtx context(numReceivers_, maxOWD_, 40, 90, maxOWD_);

    MessageHandlerFunc handleRequest = [this, &context](MessageHeader *hdr, void *body, Address *sender) {
        VLOG(5) << "Received message from " << sender->ip() << " " << (int) hdr->msgType << " " << hdr->msgLen;

        if (hdr->msgType == MessageType::MEASUREMENT_REPLY) {
            MeasurementReply reply;

            if (!reply.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse Measurement_Reply message";
                return;
            }
            uint64_t now = GetMicrosecondTimestamp();

            if (reply.owd() > 0) {
                context.addMeasure(reply.receiver_id(), reply.owd());
            } else {
                // This shouldn't matter too much, since it is ultimately the furthest/max receiver that determines
                // the deadline
                VLOG(4) << "Warning, negative OWD measurement, using RTT / 2";
                context.addMeasure(reply.receiver_id(), (now - reply.send_time()) / 2);
            }

            latencyBound_.store(context.getCappedMaxOWD() * offsetCoefficient_);
            VLOG(1) << "proxy=" << proxyId_ << " replica=" << reply.receiver_id() << " owd=" << reply.owd()
                    << " rtt=" << now - reply.send_time() << " now=" << now << "\nLatency bound is set to be "
                    << latencyBound_.load();

        } else if (!isFirstReq && proxyBatchEnabled_ && hdr->msgType == MessageType::CLIENT_REQUEST) {
            ClientRequest inReq;
            if (!inReq.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }
            domReqBatchBuffer_.emplace_back();
            DOMRequest &outReq = domReqBatchBuffer_.back();

            SetDOMRequest(inReq, outReq, hdr);

            VLOG(2) << "Buffering (" << inReq.client_id() << ", " << inReq.client_seq()
                    << ") deadline=" << outReq.deadline() << " latencyBound=" << latencyBound_
                    << " now=" << GetMicrosecondTimestamp() << " batchSize=" << domReqBatchBuffer_.size();

            uint64_t now = GetMicrosecondTimestamp();
            uint64_t firstReqSendTime = domReqBatchBuffer_.front().send_time();
            curBatchDelay_ = now - firstReqSendTime;

            // check the timeout lazily is good enought, no need to set a timer
            if (domReqBatchBuffer_.size() >= proxyBatchMaxCount_ || curBatchDelay_ >= proxyBatchMaxDelay_) {
                DOMBatchRequest batchReq;
                for (auto &domReq : domReqBatchBuffer_) {
                    DOMRequest *req = batchReq.add_requests();
                    req->Swap(&domReq);
                }
                domReqBatchBuffer_.clear();
                curBatchDelay_ = 0;
                batchReq.set_send_time(GetMicrosecondTimestamp());
                batchReq.set_proxy_id(proxyId_);
                // TODO(Hao): make sure the BUFFER_SIZE is large enough
                MessageHeader *hdr = endpoint_->PrepareProtoMsg(batchReq, MessageType::DOM_BATCH_REQUEST);
                for (int i = 0; i < numReceivers_; i++) {

                    VLOG(1) << "Forwarding batched req to " << receiverAddrs_[i].ip() << ":" << receiverAddrs_[i].port_
                            << " msgType=" << (int) hdr->msgType << " msgLen=" << hdr->msgLen;

                    endpoint_->SendPreparedMsgTo(receiverAddrs_[i], hdr);
                }
            }
        } else if (hdr->msgType == MessageType::CLIENT_REQUEST) {

            ClientRequest inReq;   // Client request we get
            DOMRequest outReq;     // Outgoing request that we attach a deadline to

            // TODO verify and handle signed header better
            if (!inReq.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

            SetDOMRequest(inReq, outReq, hdr);

            isFirstReq = false;
            VLOG(2) << "Forwarding (" << inReq.client_id() << ", " << inReq.client_seq()
                    << ") deadline=" << outReq.deadline() << " latencyBound=" << latencyBound_
                    << " now=" << GetMicrosecondTimestamp();

            if (numForwarded_ % 10000 == 0) {
                VLOG(1) << "Forwarding request number " << numForwarded_ + 1 << " at time "
                        << GetMicrosecondTimestamp();
            }
            numForwarded_++;

            MessageHeader *hdr = endpoint_->PrepareProtoMsg(outReq, MessageType::DOM_REQUEST);

            for (int i = 0; i < numReceivers_; i++) {

                VLOG(1) << "Forwarding req (" << inReq.client_id() << ", " << inReq.client_seq() << ") to "
                        << receiverAddrs_[i].ip() << ":" << receiverAddrs_[i].port_ << " msgType=" << (int) hdr->msgType
                        << " msgLen=" << hdr->msgLen;

                endpoint_->SendPreparedMsgTo(receiverAddrs_[i], hdr);
            }
        } else {
            LOG(ERROR) << "Unknown message type " << (int) hdr->msgType;
        }
    };

    endpoint_->RegisterMsgHandler(handleRequest);
    endpoint_->Connect();

    LOG(INFO) << "Forward loop starting";

    endpoint_->LoopRun();

    LOG(INFO) << "Forward loop ending";
}

Proxy::~Proxy() {}

void Proxy::sendReq(uint32_t seq)
{
    uint64_t now = GetMicrosecondTimestamp();
    uint64_t deadline = now + latencyBound_;
    deadline = std::max(deadline, lastDeadline_ + 1);
    lastDeadline_ = deadline;

    DOMRequest outReq;
    outReq.set_send_time(now);
    outReq.set_deadline(deadline);
    outReq.set_proxy_id(proxyId_);

    outReq.set_deadline_set_size(numReceivers_);
    outReq.set_late(false);

    outReq.set_client_id(proxyId_);
    outReq.set_client_seq(seq);

    VLOG(1) << "Issuing simmed client req (" << proxyId_ << ", " << seq << ") to " << " deadline=" << deadline
            << " latencyBound=" << latencyBound_ << " now=" << GetMicrosecondTimestamp();

    for (int i = 0; i < numReceivers_; i++) {
        MessageHeader *hdr = endpoint_->PrepareProtoMsg(outReq, MessageType::DOM_REQUEST);
        endpoint_->SendPreparedMsgTo(receiverAddrs_[i], hdr);
    }
}

}   // namespace dombft
