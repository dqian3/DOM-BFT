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

    selfGenReqs_ = false;

    std::string proxyKey = config.proxyKeysDir + "/proxy" + std::to_string(proxyId) + ".der";
    LOG(INFO) << "Loading key from " << proxyKey;

    if (!sigProvider_.loadPrivateKey(proxyKey)) {
        LOG(ERROR) << "Unable to load private key!";
        exit(1);
    }

    numReceivers_ = config.replicaIps.size();

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

Proxy::Proxy(const ProcessConfig &config, uint32_t proxyId, uint32_t freq, uint32_t duration, bool poisson)
    : Proxy(config, proxyId)
{
    // Setup experimental parameters
    selfGenReqs_ = true;
    genReqFreq_ = freq;
    genReqDuration_ = duration;
    genReqPoisson_ = poisson;
}

void Proxy::terminate()
{
    LOG(INFO) << "Terminating...";
    running_ = false;
}

void Proxy::run()
{
    running_ = true;
    if (selfGenReqs_) {
        GenerateRequestsTd();
    } else {
        ForwardRequests();
    }
    LOG(INFO) << "Run Terminated ";
}

Proxy::~Proxy()
{

    // TODO Cleanup more
}

void Proxy::ForwardRequests()
{
    OWDCalc::PercentileCtx context(numReceivers_, maxOWD_, 40, 90, maxOWD_);

    MessageHandlerFunc handleRequest = [this, &context](MessageHeader *hdr, void *body, Address *sender) {
        VLOG(2) << "Received message from " << sender->ip() << " " << (int) hdr->msgType << " " << hdr->msgLen;

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

        } else if (hdr->msgType == MessageType::CLIENT_REQUEST) {

            ClientRequest inReq;   // Client request we get
            DOMRequest outReq;     // Outgoing request that we attach a deadline to

            // TODO verify and handle signed header better
            if (!inReq.ParseFromArray(body, hdr->msgLen)) {
                LOG(ERROR) << "Unable to parse CLIENT_REQUEST message";
                return;
            }

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

            VLOG(2) << "Forwarding (" << inReq.client_id() << ", " << inReq.client_seq() << ") deadline=" << deadline
                    << " latencyBound=" << latencyBound_ << " now=" << GetMicrosecondTimestamp();

            if (numForwarded_ % 10000 == 0) {
                VLOG(1) << "Forwarding request number " << numForwarded_ + 1 << " at time " << now;
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
            LOG(ERROR) << "Unknown message type " << hdr->msgType;
        }
    };

    /* Checks every 10ms to see if we are done*/
    auto checkEnd = [](void *ctx, void *ep) {
        if (!((Proxy *) ctx)->running_) {
            ((Endpoint *) ep)->LoopBreak();
        }
    };

    Timer monitor(checkEnd, 10000, this);

    endpoint_->RegisterMsgHandler(handleRequest);
    endpoint_->RegisterTimer(&monitor);

    endpoint_->Connect();

    LOG(INFO) << "Forward loop starting";

    endpoint_->LoopRun();

    LOG(INFO) << "Forward loop ending";
}

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

void Proxy::GenerateRequestsTd()
{
    uint32_t seq = 0;

    // If we want to generate requests according to a poisson process with an average
    // rate of genReqFreq_, the lambda parameter should just be 1/avg interval, which
    // is just the freq.
    std::random_device rd;    // uniformly-distributed integer random number generator
    std::mt19937 rng(rd());   // mt19937: Pseudo-random number generation
    std::exponential_distribution<double> exp(genReqFreq_);

    // If request frequency is high enough, don't rely on event library, and just busy wait for next time
    // Since at frequencies above 1000/s, the timers don't trigger fast enough
    if (genReqFreq_ > 1000) {

        uint64_t now = GetMicrosecondTimestamp();
        uint64_t start = now;
        uint64_t lastSent = now;
        uint64_t nextSend = 0;

        while (now - start < genReqDuration_ * 1000000) {
            now = GetMicrosecondTimestamp();

            if (now - lastSent < nextSend) {
                continue;
            }

            sendReq(seq);
            seq++;
            lastSent = now;

            // interval in seconds between requests
            double interval = genReqPoisson_ ? exp(rng) : 1.0 / genReqFreq_;
            // convert to microseconds, but don't let it go to 0
            uint32_t interval_us = interval * 1000000;
            interval_us = std::max(1u, interval_us);
            nextSend = interval_us;
        }

        running_ = false;
        LOG(INFO) << "Ending experiment after busy-waiting";
        LOG(INFO) << "Sent " << seq << " requests";

    } else {
        Timer timer(
            [&, this](void *ctx, void *endpoint) {
                Endpoint *ep = (Endpoint *) endpoint;
                sendReq(seq);
                seq++;

                // interval in seconds between requests
                double interval = genReqPoisson_ ? exp(rng) : 1.0 / genReqFreq_;
                // convert to microseconds, but don't let it go to 0
                uint32_t interval_us = interval * 1000000;
                interval_us = std::max(1u, interval_us);

                ep->ResetTimer(&timer, interval_us);
            },
            1000, this
        );   // initial time doesn't matter, since it's reset

        Timer endExperiment(
            [&seq, this](void *ctx, void *endpoint) {
                running_ = false;
                LOG(INFO) << "Ending experiment";
                LOG(INFO) << "Sent " << seq << " requests";
                ((Endpoint *) endpoint)->LoopBreak();
            },
            genReqDuration_ * 1000000, this
        );

        /* Checks every 10ms to see if we are done*/
        auto checkEnd = [](void *ctx, void *ep) {
            if (!((Proxy *) ctx)->running_) {
                ((Endpoint *) ep)->LoopBreak();
            }
        };

        Timer monitor(checkEnd, 10000, this);

        endpoint_->RegisterTimer(&timer);
        endpoint_->RegisterTimer(&monitor);

        endpoint_->RegisterTimer(&endExperiment);
        endpoint_->LoopRun();
    }
}

}   // namespace dombft
