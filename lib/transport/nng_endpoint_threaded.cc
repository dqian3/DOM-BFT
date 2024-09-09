#include "lib/transport/nng_endpoint_threaded.h"

#include <nng/protocol/pair0/pair.h>

/*************************** NngSendThread ***************************/

NngSendThread::NngSendThread(nng_socket sock, const Address &addr)
    : sock_(sock)
    , addr_(addr)
{
    LOG(INFO) << "NngEndpointThreaded semd thread started for!" << addr;

    thread_ = std::thread(&NngSendThread::run, this);
}

NngSendThread::~NngSendThread()
{
    running_ = false;
    thread_.join();
}

void NngSendThread::run()
{
    std::vector<byte> msg;

    while (running_) {
        if (queue_.try_dequeue(msg)) {
            int ret = nng_send(sock_, msg.data(), msg.size(), 0);
            if (ret != 0) {
                VLOG(1) << "\tSend to " << addr_.ip() << " failed: " << nng_strerror(ret) << " (" << ret << ")";
                continue;
            }
            VLOG(4) << "Sent to " << addr_;
        }
    }
}

void NngSendThread::sendMsg(const byte *msg, size_t len)
{
    // TODO send signal that this is ready
    VLOG(6) << "Enqueuing message for sending!";
    queue_.enqueue(std::vector<byte>{msg, msg + len});
}

/*************************** NngRecvThread ***************************/
NngRecvThread::NngRecvThread(const std::vector<nng_socket> &socks, const std::unordered_map<int, Address> &sockToAddr,
                             struct ev_loop *parentLoop, ev_async *recvWatcher)
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

        LOG(INFO) << "Registering handle for " << connAddr.ip() << ":" << connAddr.port();

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
            VLOG(5) << "Received message of length " << len << " from " << data->addr;

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
    ev_async_send(evLoop_, &stopWatcher_);
    thread_.join();

    // TODO clean up event loop properly
    // But also this thread only ends when the calling program so it's kind of fine :)
}

void NngRecvThread::run()
{
    LOG(INFO) << "NngEndpointThreaded recv thread started!";

    ev_async_init(&stopWatcher_, [](struct ev_loop *loop, ev_async *w, int revents) {
        // Signal to stop the event loop
        ev_break(loop);
    });

    ev_run(evLoop_, 0);
}

/*************************** NngEndpointThreaded ***************************/
NngEndpointThreaded::NngEndpointThreaded(const std::vector<std::pair<Address, Address>> &addrPairs,
                                         bool isMasterReceiver)
    : NngEndpoint(addrPairs, isMasterReceiver)
{
    // Parent class initializes sockets and address state

    // Initialize threads
    for (size_t i = 0; i < socks_.size(); i++) {
        sendThreads_.push_back(std::make_unique<NngSendThread>(socks_[i], socketIdxToAddr_[i]));
    }

    recvThread_ = std::make_unique<NngRecvThread>(socks_, socketIdxToAddr_, evLoop_, &recvWatcher_);
}

NngEndpointThreaded::~NngEndpointThreaded() {}

int NngEndpointThreaded::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *) sendBuffer_;

    if (addrToSocketIdx_.count(dstAddr) == 0) {
        LOG(ERROR) << "Attempt to send to unregistered address " << dstAddr.ip_ << ":" << dstAddr.port_;
        return -1;
    }

    int i = addrToSocketIdx_[dstAddr];

    sendThreads_[i]->sendMsg(sendBuffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen);
    return 0;
}

bool NngEndpointThreaded::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    hdlrFunc_ = hdl;
    recvWatcher_.data = this;

    auto cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        NngEndpointThreaded *ep = (NngEndpointThreaded *) w->data;

        std::pair<std::vector<byte>, Address> item;
        ep->recvThread_->queue_.wait_dequeue(item);

        auto &[msg, addr] = item;
        size_t len = msg.size();

        if (len > sizeof(MessageHeader)) {
            MessageHeader *hdr = (MessageHeader *) msg.data();
            if (len >= sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen) {
                ep->hdlrFunc_(hdr, msg.data() + sizeof(MessageHeader), &addr);
            }
        }
    };

    ev_async_init(&recvWatcher_, cb);
    ev_async_start(evLoop_, &recvWatcher_);
    return true;
}
