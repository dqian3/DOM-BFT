#include "lib/transport/nng_endpoint_thread.h"

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

NngEndpointThread::NngEndpointThread(const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver)
    : NngEndpoint(addrPairs, isMasterReceiver)
{
    // Use parent class to initialize sockets
}

NngEndpointThread::~NngEndpointThread()
{
    for (nng_socket sock : socks_) {
        nng_close(sock);
    }
}

int NngEndpointThread::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *) sendBuffer_;

    if (addrToSocket_.count(dstAddr) == 0) {
        LOG(ERROR) << "Attempt to send to unregistered address " << dstAddr.ip_ << ":" << dstAddr.port_;
        return -1;
    }

    nng_socket s = socks_[addrToSocket_[dstAddr]];
    int ret = nng_send(s, sendBuffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    if (ret != 0) {
        VLOG(1) << "\tSend to " << dstAddr.ip_ << " failed: " << nng_strerror(ret) << " (" << ret << ")";
        return ret;
    }

    VLOG(4) << "Sent to " << socketToAddr_[addrToSocket_[dstAddr]].GetIPAsString();
    return 0;
}

bool NngEndpointThread::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    int ret;

    // We bind separate NngMessageHandler to each socket, each with a different context,
    // so that the handler can identify which socket it
    for (uint32_t i = 0; i < socks_.size(); i++) {
        nng_socket sock = socks_[i];
        Address &connAddr = socketToAddr_[i];

        LOG(INFO) << "Registering handle for " << connAddr.GetIPAsString() << ":" << connAddr.GetPortAsInt();

        int fd;
        if ((ret = nng_socket_get_int(sock, NNG_OPT_RECVFD, &fd)) != 0) {
            nng_close(sock);
            LOG(ERROR) << "Error getting recv fd: " << nng_strerror(ret);
        }

        // TODO recvBuffer is passed as a raw pointer here
        handlers_.push_back(std::make_unique<NngMessageHandler>(hdl, sock, connAddr, recvBuffer_));
        ev_io_set(handlers_.back()->evWatcher_.get(), fd, EV_READ);
        ev_io_start(evLoop_, handlers_.back()->evWatcher_.get());
    }

    return true;
}
