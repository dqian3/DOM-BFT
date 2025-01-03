#include "lib/transport/udp_endpoint.h"

UDPMessageHandler::UDPMessageHandler(MessageHandlerFunc msghdl)
    : msgHandler_(msghdl)
{
    evWatcher_ = new ev_io();
    evWatcher_->data = (void *) this;

    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        UDPMessageHandler *m = (UDPMessageHandler *) (w->data);
        struct sockaddr_in sockAddr;
        memset(&sockAddr, 0, sizeof(sockAddr));
        socklen_t sockAddrLen = sizeof(sockAddr);

        int msgLen = recvfrom(w->fd, m->recvBuffer_, UDP_BUFFER_SIZE, 0, (struct sockaddr *) &sockAddr, &sockAddrLen);

        m->sender_ = Address(inet_ntoa(sockAddr.sin_addr), ntohs(sockAddr.sin_port));

        // LOG(INFO) << "sockAddr.sin_family: " << m->sender_.addr_.sin_family
        //           << ", sockAddr.sin_addr: " << inet_ntoa(m->sender_.addr_.sin_addr)
        //           << ", sockAddr.sin_port: " << ntohs(m->sender_.addr_.sin_port);

        // LOG(INFO) << m->sender_;

        if (msgLen > 0 && (uint32_t) msgLen > sizeof(MessageHeader)) {
            MessageHeader *msgHeader = (MessageHeader *) (void *) (m->recvBuffer_);
            if ((uint32_t) msgLen >= sizeof(MessageHeader) + msgHeader->msgLen + msgHeader->sigLen) {
                m->msgHandler_(msgHeader, m->recvBuffer_ + sizeof(MessageHeader), &m->sender_);
            }
        }
    });
}

UDPMessageHandler::~UDPMessageHandler() {}

UDPEndpoint::UDPEndpoint(
    const std::string &ip, const int port, const bool isMasterReceiver, const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
{
    fd_ = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        LOG(ERROR) << "Receiver Fd fail ";
        return;
    }
    // Set Non-Blocking
    int status = fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) {
        LOG(ERROR) << " Set NonBlocking Fail";
    }
    if (ip == "" || port < 0) {
        return;
    }

    bound_ = true;
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    // Bind socket to Address
    int bindRet = bind(fd_, (struct sockaddr *) &addr, sizeof(addr));
    if (bindRet != 0) {
        LOG(ERROR) << "bind error\t" << bindRet << "\t port=" << port;
        return;
    }
}

UDPEndpoint::~UDPEndpoint() {}

int UDPEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    int ret = sendto(
        fd_, hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0, (struct sockaddr *) (&(dstAddr.addr_)),
        sizeof(sockaddr_in)
    );
    if (ret < 0) {
        VLOG(1) << "\tSend Fail: " << strerror(errno);
        VLOG(1) << "Message size: " << (sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen);
        VLOG(1) << "Address: " << dstAddr;
    }

    return ret;
}

bool UDPEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    if (!bound_) {
        LOG(ERROR) << "Endpoint is not bound!";
        return false;
    }
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    msgHandler_ = std::make_unique<UDPMessageHandler>(hdl);
    ev_io_set(msgHandler_->evWatcher_, fd_, EV_READ);
    ev_io_start(evLoop_, msgHandler_->evWatcher_);

    return true;
}
