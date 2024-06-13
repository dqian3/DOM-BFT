#include "lib/transport/nng_endpoint.h"

#include <nng/protocol/pair0/pair.h>

NngMessageHandler::NngMessageHandler(MessageHandlerFunc msghdl, nng_socket s, const Address &otherAddr, byte *recvBuffer)
    : handlerFunc_(msghdl)
    , sock_(s)
    , srcAddr_(otherAddr)
    , recvBuffer_(recvBuffer)
    , evWatcher_(std::make_unique<ev_io>())
{
    evWatcher_->data = (void *)this;

    ev_init(evWatcher_.get(), [](struct ev_loop *loop, struct ev_io *w, int revents) {
        NngMessageHandler* m = (NngMessageHandler*)(w->data);
        int ret;
        size_t len = NNG_BUFFER_SIZE;
        
        if ((ret = nng_recv(m->sock_, m->recvBuffer_, &len, 0)) != 0) {
            LOG(ERROR) << "nng_recv failure: " << nng_strerror(ret);
            return;
        }
        
        
        if (len > sizeof(MessageHeader)) 
        {
            MessageHeader* hdr = (MessageHeader*)(void*)(m->recvBuffer_);
            if (len >= sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen) 
            {
                m->handlerFunc_(hdr, m->recvBuffer_ + sizeof(MessageHeader),
                                &(m->srcAddr_));
            }
        }

    });
}

NngMessageHandler::~NngMessageHandler() {
}

NngEndpoint::NngEndpoint(const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver)
    : Endpoint(isMasterReceiver)
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

        socks_.push_back(sock);
        addrToSocket_[connAddr] = socks_.size() - 1;
        socketToAddr_[socks_.size() - 1] = connAddr;

    } 

}

NngEndpoint::~NngEndpoint() {
    for (nng_socket sock : socks_) {
        nng_close(sock);
    }
}

int NngEndpoint::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *)sendBuffer_;

    nng_socket s = socks_[addrToSocket_[dstAddr]];
    int ret = nng_send(s, sendBuffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    if (ret != 0)
    {
        VLOG(1) << "\tSend Fail " << nng_strerror(ret);
    }
    return 0;
}

bool NngEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    int ret;

    // We bind separate NngMessageHandler to each socket, each with a different context, 
    // so that the handler can identify which socket it 
    for (int i = 0; i < socks_.size(); i++) {
        nng_socket sock = socks_[i];
        Address &connAddr = socketToAddr_[i];

        LOG(INFO) << "Registering handle for " << connAddr.GetIPAsString();

        int fd;
        if ((ret = nng_socket_get_int(sock, NNG_OPT_RECVFD, &fd)) != 0) {
            nng_close(sock);
            LOG(ERROR) << "Error getting recv fd: "  << nng_strerror(ret);
        }

        // TODO recvBuffer is passed as a raw pointer here
        handlers_.push_back(std::make_unique<NngMessageHandler>(hdl, sock, connAddr, recvBuffer_));
        ev_io_set(handlers_.back()->evWatcher_.get(), fd, EV_READ);
        ev_io_start(evLoop_, handlers_.back()->evWatcher_.get());


    }


    return true;
}
