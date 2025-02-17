#include "lib/transport/nng_endpoint.h"

#include <nng/protocol/pair0/pair.h>

NngMessageHandler::NngMessageHandler(
    MessageHandlerFunc msghdl, nng_socket s, const Address &otherAddr, byte *recvBuffer
)
    : handlerFunc_(msghdl)
    , sock_(s)
    , srcAddr_(otherAddr)
    , recvBuffer_(recvBuffer)
    , evWatcher_()
{
    evWatcher_.data = (void *) this;

    ev_init(&evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        NngMessageHandler *m = (NngMessageHandler *) (w->data);
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

NngMessageHandler::~NngMessageHandler() {}

NngEndpoint::NngEndpoint(
    const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver,
    const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
{
    int ret;

    for (auto &[bindAddr, connAddr] : addrPairs) {
        nng_socket sock = NNG_SOCKET_INITIALIZER;

        if ((ret = nng_pair_open(&sock)) != 0) {
            LOG(ERROR) << "Failed to open socket " << nng_strerror(ret);
            return;
        }

        // TODO other protocols?
        std::string bindUrl = "tcp://" + bindAddr.ip_ + ":" + std::to_string(bindAddr.port_);
        std::string sendUrl = "tcp://" + connAddr.ip_ + ":" + std::to_string(connAddr.port_);

        if ((ret = nng_listen(sock, bindUrl.c_str(), NULL, 0)) != 0) {
            LOG(ERROR) << "Failed to bind to " << bindUrl << ": " << nng_strerror(ret);
            return;
        }

        if ((ret = nng_dial(sock, sendUrl.c_str(), NULL, 0)) != 0) {
            LOG(ERROR) << "Failed to connect to " << sendUrl << ": " << nng_strerror(ret);
            // return;
        }

        VLOG(1) << bindUrl << " <---> " << sendUrl;

        socks_.push_back(sock);
        addrToSocketIdx_[connAddr] = socks_.size() - 1;
        socketIdxToAddr_[socks_.size() - 1] = connAddr;
    }
}

NngEndpoint::~NngEndpoint()
{
    for (nng_socket sock : socks_) {
        nng_close(sock);
    }
}

int NngEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    if (addrToSocketIdx_.count(dstAddr) == 0) {
        LOG(ERROR) << "Attempt to send to unregistered address " << dstAddr.ip_ << ":" << dstAddr.port_;
        return -1;
    }

    nng_socket s = socks_[addrToSocketIdx_[dstAddr]];
    int ret = nng_send(s, hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    if (ret != 0) {
        VLOG(1) << "\tSend to " << dstAddr.ip_ << " failed: " << nng_strerror(ret) << " (" << ret << ")";
        return ret;
    }

    VLOG(4) << "Sent to " << socketIdxToAddr_[addrToSocketIdx_[dstAddr]].ip();
    return 0;
}

bool NngEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    int ret;

    // We bind separate NngMessageHandler to each socket, each with a different context,
    // so that the handler can identify which socket it
    for (uint32_t i = 0; i < socks_.size(); i++) {
        nng_socket sock = socks_[i];
        Address &connAddr = socketIdxToAddr_[i];

        LOG(INFO) << "Registering handle for " << connAddr.ip() << ":" << connAddr.port();

        int fd;
        if ((ret = nng_socket_get_int(sock, NNG_OPT_RECVFD, &fd)) != 0) {
            nng_close(sock);
            LOG(ERROR) << "Error getting recv fd: " << nng_strerror(ret);
        }

        // TODO recvBuffer is passed as a raw pointer here
        handlers_.push_back(std::make_unique<NngMessageHandler>(hdl, sock, connAddr, recvBuffer_));
        ev_io_set(&handlers_.back()->evWatcher_, fd, EV_READ);
        ev_io_start(evLoop_, &handlers_.back()->evWatcher_);
    }

    return true;
}
