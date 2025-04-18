#include "lib/transport/nng_endpoint_threaded.h"

#include <nng/protocol/pair0/pair.h>

/*************************** NngSendThread ***************************/

NngSendThread::NngSendThread(nng_socket sock, const Address &addr)
    : sock_(sock)
    , addr_(addr)
{
    LOG(INFO) << "NngEndpointThreaded send thread started for " << addr;

    evLoop_ = ev_loop_new();
    sendWatcher_ = ev_async();
    stopWatcher_ = ev_async();

    // Timeout after 5 seconds, so we can gracefully exit
    // TODO would be nice to do something neater like handle queues/timeouts ourselves.
    nng_setopt_ms(sock, NNG_OPT_SENDTIMEO, 5000);

    thread_ = std::thread(&NngSendThread::run, this);
}

NngSendThread::~NngSendThread()
{
    if (thread_.joinable()) {
        ev_async_send(evLoop_, &stopWatcher_);
        thread_.join();
    }
}

void NngSendThread::run()
{
    auto stop_cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        // Signal to stop the event loop
        ev_break(loop, EVBREAK_ALL);
    };

    sendWatcher_.data = this;
    auto send_cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        NngSendThread *t = (NngSendThread *) w->data;

        uint32_t BATCH_SIZE = 5;
        std::vector<std::vector<byte>> msgs(BATCH_SIZE);
        size_t numMsgs;
        while (numMsgs = t->queue_.wait_dequeue_bulk_timed(msgs.begin(), 5, 50000)) {
            if (numMsgs > 1) {

                VLOG(6) << "Batching  " << numMsgs << " nng messages to " << t->addr_;

                // Send up to 5 messages. However, if the messages get too big, we may need to split them up..
                std::vector<byte> &curMsg = msgs[0];
                curMsg.reserve(curMsg.size() * 5);

                for (int i = 1; i < numMsgs; i++) {
                    // Next message is too big... Send current batch and start new one
                    if (curMsg.size() + msgs[i].size() > NNG_BUFFER_SIZE) {
                        int ret = nng_send(t->sock_, curMsg.data(), curMsg.size(), 0);
                        if (ret != 0) {
                            LOG(ERROR) << "\tSend to " << t->addr_ << " failed: " << nng_strerror(ret) << " (" << ret
                                       << ")";
                            return;
                        }

                        curMsg = msgs[i];
                        continue;
                    }

                    // Move next message into curMsg vector
                    curMsg.insert(curMsg.end(), msgs[i].begin(), msgs[i].end());
                }

                // Send last batch
                int ret = nng_send(t->sock_, curMsg.data(), curMsg.size(), 0);
                if (ret != 0) {
                    VLOG(1) << "\tSend to " << t->addr_ << " failed: " << nng_strerror(ret) << " (" << ret << ")";
                    return;
                }

            } else {
                int ret = nng_send(t->sock_, msgs[0].data(), msgs[0].size(), 0);
                if (ret != 0) {
                    VLOG(1) << "\tSend to " << t->addr_ << " failed: " << nng_strerror(ret) << " (" << ret << ")";
                    return;
                }
                VLOG(6) << "Sent to " << t->addr_;
            }
        }
    };

    ev_async_init(&stopWatcher_, stop_cb);
    ev_async_init(&sendWatcher_, send_cb);

    ev_async_start(evLoop_, &stopWatcher_);
    ev_async_start(evLoop_, &sendWatcher_);

    ev_set_priority(&stopWatcher_, EV_MAXPRI);

    ev_run(evLoop_, 0);
}

void NngSendThread::sendMsg(const byte *msg, size_t len)
{
    queue_.enqueue(std::vector<byte>{msg, msg + len});
    ev_async_send(evLoop_, &sendWatcher_);
}

/*************************** NngRecvThread ***************************/
NngRecvThread::NngRecvThread(
    const std::vector<nng_socket> &socks, const std::unordered_map<int, Address> &sockToAddr,
    struct ev_loop *parentLoop, ev_async *recvWatcher
)
    : socks_(socks)
    , parentLoop_(parentLoop)
    , parentRecvWatcher_(recvWatcher)
{
    evLoop_ = ev_loop_new();
    ioWatchers_.resize(socks.size());

    int ret;
    int fd;

    for (size_t i = 0; i < socks_.size(); i++) {
        nng_socket sock = socks_[i];
        const Address &connAddr = sockToAddr.at(i);

        LOG(INFO) << "Registering handle for " << connAddr;

        if ((ret = nng_socket_get_int(sock, NNG_OPT_RECVFD, &fd)) != 0) {
            nng_close(sock);
            LOG(ERROR) << "Error getting recv fd: " << nng_strerror(ret);
            return;
        }

        ev_io *watcher = &ioWatchers_[i];
        watcher->data = new IOWatcherData(this, sock, connAddr);

        auto cb = [](struct ev_loop *loop, struct ev_io *w, int revents) {
            IOWatcherData *data = (IOWatcherData *) w->data;
            NngRecvThread *r = data->r;
            int ret;
            size_t len = NNG_BUFFER_SIZE;

            if ((ret = nng_recv(data->sock, r->recvBuffer_, &len, 0)) != 0) {
                LOG(ERROR) << "nng_recv failure: " << nng_strerror(ret);
                return;
            }
            VLOG(6) << "Received message of length " << len << " from " << data->addr;

            r->queue_.enqueue({std::vector<byte>(r->recvBuffer_, r->recvBuffer_ + len), data->addr});

            ev_async_send(r->parentLoop_, r->parentRecvWatcher_);
        };

        ev_init(watcher, cb);
        ev_io_set(watcher, fd, EV_READ);
        ev_io_start(evLoop_, watcher);
    }

    thread_ = std::thread(&NngRecvThread::run, this);
}

NngRecvThread::~NngRecvThread()
{
    if (thread_.joinable()) {
        ev_async_send(evLoop_, &stopWatcher_);
        thread_.join();
    }
}

void NngRecvThread::run()
{
    // LOG(INFO) << "NngEndpointThreaded recv thread started!";

    ev_async_init(&stopWatcher_, [](struct ev_loop *loop, ev_async *w, int revents) {
        // Signal to stop the event loop

        ev_break(loop, EVBREAK_ALL);
    });

    ev_async_start(evLoop_, &stopWatcher_);
    ev_set_priority(&stopWatcher_, EV_MAXPRI);

    ev_run(evLoop_, 0);

    // LOG(INFO) << "NngEndpointThreaded recv thread finished!";
}

/*************************** NngEndpointThreaded ***************************/
NngEndpointThreaded::NngEndpointThreaded(
    const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver,
    const std::optional<Address> &loopbackAddr
)
    : NngEndpoint(addrPairs, isMasterReceiver, loopbackAddr)
{
    // Parent class initializes sockets and address state

    // Initialize threads
    for (size_t i = 0; i < socks_.size(); i++) {
        sendThreads_.push_back(std::make_unique<NngSendThread>(socks_[i], socketIdxToAddr_[i]));
    }

    recvThread_ = std::make_unique<NngRecvThread>(socks_, socketIdxToAddr_, evLoop_, &recvWatcher_);
}

NngEndpointThreaded::~NngEndpointThreaded() {}

void NngEndpointThreaded::setCpuAffinities(const std::set<int> &recvCpus, const std::set<int> &sendCpus)
{

    // Either both or neither should be set
    assert(!recvCpus.empty() && !sendCpus.empty());
    if (recvCpus.empty()) {
        cpu_set_t sendSet;
        cpu_set_t recvSet;

        CPU_ZERO(&sendSet);
        CPU_ZERO(&recvSet);

        for (int cpu : sendCpus) {
            CPU_SET(cpu, &sendSet);
        }
        for (int cpu : recvCpus) {
            CPU_SET(cpu, &recvSet);
        }

        // TODO portability and error codes
        for (size_t i = 0; i < sendThreads_.size(); i++) {
            int ret = pthread_setaffinity_np(sendThreads_[i]->thread_.native_handle(), sizeof(sendSet), &sendSet);
            if (ret != 0) {
                LOG(ERROR) << "Error setting thread affinity for send thread: " << ret;
                exit(1);
            }
        }

        int ret = pthread_setaffinity_np(recvThread_->thread_.native_handle(), sizeof(recvSet), &recvSet);
        if (ret != 0) {
            LOG(ERROR) << "Error setting thread affinity for recv thread: " << ret;
            exit(1);
        }
    }
}

int NngEndpointThreaded::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    // check for loopback
    if (loopbackAddr_.has_value() && dstAddr == loopbackAddr_.value()) {
        byte *msg = (byte *) hdr;
        recvThread_->queue_.enqueue(
            {std::vector<byte>(msg, msg + sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen), dstAddr}
        );
        ev_async_send(evLoop_, &recvWatcher_);
        return 0;
    }

    if (addrToSocketIdx_.count(dstAddr) == 0) {
        LOG(ERROR) << "Attempt to send to unregistered address " << dstAddr.ip_ << ":" << dstAddr.port_;
        return -1;
    }

    int i = addrToSocketIdx_[dstAddr];

    sendThreads_[i]->sendMsg((byte *) hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen);
    return 0;
}

bool NngEndpointThreaded::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    hdlrFunc_ = hdl;
    recvWatcher_.data = this;

    auto cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        NngEndpointThreaded *ep = (NngEndpointThreaded *) w->data;
        std::pair<std::vector<byte>, Address> item;

        while (ep->recvThread_->queue_.try_dequeue(item)) {
            auto &[msg, addr] = item;
            size_t totalLen = msg.size();
            size_t offset = 0;

            while (totalLen - offset > sizeof(MessageHeader)) {
                byte *msgStart = msg.data() + offset;
                MessageHeader *hdr = (MessageHeader *) (msgStart);
                size_t msgLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;

                if (offset + msgLen <= totalLen) {
                    ep->hdlrFunc_(hdr, msgStart + sizeof(MessageHeader), &addr);
                } else {
                    LOG(WARNING) << "Malformed message " << totalLen << " " << offset << " " << msgLen;
                }

                offset += msgLen;
            }
        }
    };

    ev_async_init(&recvWatcher_, cb);
    ev_async_start(evLoop_, &recvWatcher_);
    return true;
}

void NngEndpointThreaded::LoopBreak()
{
    LOG(INFO) << "NngEndpointThreaded::LoopBreak() called";

    for (auto &t : sendThreads_) {
        ev_async_send(t->evLoop_, &t->stopWatcher_);
    }

    ev_async_send(recvThread_->evLoop_, &recvThread_->stopWatcher_);

    for (auto &t : sendThreads_) {
        t->thread_.join();
        LOG(INFO) << "Join send thread " << t->addr_;
    }

    recvThread_->thread_.join();
    LOG(INFO) << "Join recv thread";

    Endpoint::LoopBreak();
}