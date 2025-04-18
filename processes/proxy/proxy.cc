#include "proxy.h"

#include "lib/transport/nng_endpoint_threaded.h"

namespace dombft {
using namespace dombft::proto;

Proxy::Proxy(const ProcessConfig &config, uint32_t proxyId)
{
    numShards_ = config.proxyShards;
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

    numReceivers_ = config.receiverIps.size();

    if (config.transport == "nng") {
        if (numShards_ > 1) {
            LOG(ERROR) << "Multiple shards for proxy and NNG not implemented yet!";
            exit(1);
        }
        auto addrPairs = getProxyAddrs(config, proxyId);

        // This is rather messy, but the last nReceivers addresses in this return value are for the measurement
        // connections
        size_t nClients = config.clientIps.size();
        size_t nReplicas = config.replicaIps.size();
        std::vector<std::pair<Address, Address>> forwardAddrs(
            addrPairs.begin(), addrPairs.end() - config.receiverIps.size()
        );
        std::vector<std::pair<Address, Address>> measurementAddrs(
            addrPairs.end() - config.receiverIps.size(), addrPairs.end()
        );

        forwardEps_.push_back(std::make_unique<NngEndpointThreaded>(forwardAddrs, false));
        measurementEp_ = std::make_unique<NngEndpointThreaded>(measurementAddrs);

        for (size_t i = nClients; i < forwardAddrs.size(); i++) {
            receiverAddrs_.push_back(forwardAddrs[i].second);
        }

    } else {
        for (int i = 0; i < numShards_; i++) {
            forwardEps_.push_back(
                std::make_unique<UDPEndpoint>(config.proxyIps[proxyId], config.proxyForwardPort + i, false)
            );
        }

        measurementEp_ = std::make_unique<UDPEndpoint>(config.proxyIps[proxyId], config.proxyMeasurementPort);

        for (int i = 0; i < numReceivers_; i++) {
            std::string receiverIp = config.receiverIps[i];
            receiverAddrs_.push_back(Address(receiverIp, config.receiverPort));
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

    LaunchThreads();
    for (auto &kv : threads_) {
        LOG(INFO) << "Join " << kv.first;
        kv.second->join();
        LOG(INFO) << "Join Complete " << kv.first;
    }

    LOG(INFO) << "Run Terminated ";
}

Proxy::~Proxy()
{

    // TODO Cleanup more
}

void Proxy::LaunchThreads()
{
    threads_["RecvMeasurementsTd"] = std::make_unique<std::thread>(&Proxy::RecvMeasurementsTd, this);
    if (selfGenReqs_) {
        threads_["GenerateRequestsTd"] = std::make_unique<std::thread>(&Proxy::GenerateRequestsTd, this);
    } else {
        for (int i = 0; i < numShards_; i++) {
            std::string key = "ForwardRequestsTd-" + std::to_string(i);
            threads_[key] = std::make_unique<std::thread>(&Proxy::ForwardRequestsTd, this, i);
        }
    }
}

void Proxy::RecvMeasurementsTd()
{
    OWDCalc::PercentileCtx context(numReceivers_, maxOWD_, 40, 90, maxOWD_);
    // OWDCalc::MaxCtx context(numReceivers_, maxOWD_);

    MessageHandlerFunc handleMeasurementReply = [this, &context](MessageHeader *hdr, void *body, Address *sender) {
        MeasurementReply reply;

        if (!reply.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse Measurement_Reply message";
            return;
        }
        uint64_t now = GetMicrosecondTimestamp();

        if (reply.owd() > 0) {
            context.addMeasure(reply.receiver_id(), reply.owd());
        } else {
            // THis shouldn't matter too much, since it is ultimately the furtherest/max recevier that determines
            // the deadline
            VLOG(4) << "Warning, negative OWD measurement, using RTT / 2";
            context.addMeasure(reply.receiver_id(), (now - reply.send_time()) / 2);
        }

        latencyBound_.store(context.getCappedMaxOWD() * offsetCoefficient_);
        VLOG(1) << "proxy=" << proxyId_ << " replica=" << reply.receiver_id() << " owd=" << reply.owd()
                << " rtt=" << now - reply.send_time() << " now=" << now << "\nLatency bound is set to be "
                << latencyBound_.load();
    };

    /* Checks every 10ms to see if we are done*/
    auto checkEnd = [](void *ctx, void *ep) {
        if (!((Proxy *) ctx)->running_) {
            ((Endpoint *) ep)->LoopBreak();
        }
    };

    Timer monitor(checkEnd, 10000, this);

    measurementEp_->RegisterMsgHandler(handleMeasurementReply);
    measurementEp_->RegisterTimer(&monitor);

    measurementEp_->LoopRun();

    LOG(INFO) << "Measurement thread ending";
}

void Proxy::ForwardRequestsTd(const int thread_id)
{
    MessageHandlerFunc handleClientRequest = [this, thread_id](MessageHeader *hdr, void *body, Address *sender) {
        ClientRequest inReq;   // Client request we get
        DOMRequest outReq;     // Outgoing request that we attach a deadline to

        VLOG(2) << "Received message from " << sender->ip() << " " << (int) hdr->msgType << " " << hdr->msgLen;

        if (hdr->msgType == MessageType::CLIENT_REQUEST) {
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

            MessageHeader *hdr = forwardEps_[thread_id]->PrepareProtoMsg(outReq, MessageType::DOM_REQUEST);
#if FABRIC_CRYPTO
            sigProvider_.appendSignature(hdr, SEND_BUFFER_SIZE);
#endif

            for (int i = 0; i < numReceivers_; i++) {

                forwardEps_[thread_id]->SendPreparedMsgTo(receiverAddrs_[i]);
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

    forwardEps_[thread_id]->RegisterMsgHandler(handleClientRequest);
    forwardEps_[thread_id]->RegisterTimer(&monitor);

    forwardEps_[thread_id]->LoopRun();

    LOG(INFO) << "Forward thread ending";
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
        MessageHeader *hdr = forwardEps_[0]->PrepareProtoMsg(outReq, MessageType::DOM_REQUEST);
        forwardEps_[0]->SendPreparedMsgTo(receiverAddrs_[i]);
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

        forwardEps_[0]->RegisterTimer(&timer);
        forwardEps_[0]->RegisterTimer(&monitor);

        forwardEps_[0]->RegisterTimer(&endExperiment);
        forwardEps_[0]->LoopRun();
    }
}

}   // namespace dombft
