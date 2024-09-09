#include "lib/transport/nng_endpoint_thread.h"

#include "nng_endpoint_thread.h"
#include <nng/protocol/pair0/pair.h>

NngThreadMessageHandler::NngThreadMessageHandler(MessageHandlerFunc msghdl, nng_socket s, const Address &otherAddr,
                                                 byte *recvBuffer)
    : handlerFunc_(msghdl)
    , sock_(s)
    , srcAddr_(otherAddr)
    , recvBuffer_(recvBuffer)
    , evWatcher_(std::make_unique<ev_io>())
{
    evWatcher_->data = (void *) this;

    ev_init(evWatcher_.get(), [](struct ev_loop *loop, struct ev_io *w, int revents) {
        NngThreadMessageHandler *m = (NngThreadMessageHandler *) (w->data);
        int ret;
        size_t len = NNG_BUFFER_SIZE;

        if ((ret = nng_recv(m->sock_, m->recvBuffer_, &len, 0)) != 0) {
            LOG(ERROR) << "nng_recv failure: " << nng_strerror(ret);
            return;
        }

        if (len > sizeof(MessageHeader)) {
            MessageHeader *hdr = (MessageHeader *) (void *) (m->recvBuffer_);
            if (len >= sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen) {
                m->handlerFunc_(hdr, m->recvBuffer_ + sizeof(MessageHeader), &(m->srcAddr_));
            }
        }
    });
}

NngThreadMessageHandler::~NngThreadMessageHandler() {}

NngEndpointThreaded::NngEndpointThreaded(const std::vector<std::pair<Address, Address>> &addrPairs,
                                         bool isMasterReceiver)
    : NngEndpoint(addrPairs, isMasterReceiver)
{
    // Parent class initializes sockets

    // Here we initialize threads
}

NngEndpointThreaded::~NngEndpointThreaded() {}

int NngEndpointThreaded::SendPreparedMsgTo(const Address &dstAddr)
{
    // MessageHeader *hdr = (MessageHeader *) sendBuffer_;

    // if (addrToSocket_.count(dstAddr) == 0) {
    //     LOG(ERROR) << "Attempt to send to unregistered address " << dstAddr.ip_ << ":" << dstAddr.port_;
    //     return -1;
    // }

    // nng_socket s = socks_[addrToSocket_[dstAddr]];
    // int ret = nng_send(s, sendBuffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    // if (ret != 0) {
    //     VLOG(1) << "\tSend to " << dstAddr.ip_ << " failed: " << nng_strerror(ret) << " (" << ret << ")";
    //     return ret;
    // }

    // VLOG(4) << "Sent to " << socketToAddr_[addrToSocket_[dstAddr]].GetIPAsString();
    return 0;
}

bool NngEndpointThreaded::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    // int ret;

    // // We bind separate NngMessageHandler to each socket, each with a different context,
    // // so that the handler can identify which socket it
    // for (uint32_t i = 0; i < socks_.size(); i++) {
    //     nng_socket sock = socks_[i];
    //     Address &connAddr = socketToAddr_[i];

    //     LOG(INFO) << "Registering handle for " << connAddr.GetIPAsString() << ":" << connAddr.GetPortAsInt();

    //     int fd;
    //     if ((ret = nng_socket_get_int(sock, NNG_OPT_RECVFD, &fd)) != 0) {
    //         nng_close(sock);
    //         LOG(ERROR) << "Error getting recv fd: " << nng_strerror(ret);
    //     }

    //     // TODO recvBuffer is passed as a raw pointer here
    //     handlers_.push_back(std::make_unique<NngMessageHandler>(hdl, sock, connAddr, recvBuffer_));
    //     ev_io_set(handlers_.back()->evWatcher_.get(), fd, EV_READ);
    //     ev_io_start(evLoop_, handlers_.back()->evWatcher_.get());
    // }

    return true;
}

NngSendThread::NngSendThread(nng_socket sock, const Address &addr)
    : sock_(sock)
    , addr_(addr)
{
    // TODO use libev async instead of busy waiting here
    thread_ = std::thread(&NngSendThread::run, this);
}

NngSendThread::~NngSendThread()
{
    running_ = false;
    thread_.join();
}

NngSendThread::run()
{
    std::vector<byte> msg;

    while (running_) {
        if (queue_.try_dequeue(msg)) {
            int ret = nng_send(sock_, msg.data(), msg.size(), 0);
            if (ret != 0) {
                VLOG(1) << "\tSend to " << addr_.GetIpAsString() << " failed: " << nng_strerror(ret) << " (" << ret
                        << ")";
                return ret;
            }
            VLOG(4) << "Sent to " << addr_.GetIPAsString();
        }
    }
}

NngSendThread::sendMsgTo(const byte *msg, size_t len) { queue_.enqueue_back(std::vector<byte>{msg, msg + len}); }

NngRecvThread::NngRecvThread(std::vector<nng_socket> socks_) {}

bool NngRecvThread::registerMsgHandler(MessageHandlerFunc) { return false; }
