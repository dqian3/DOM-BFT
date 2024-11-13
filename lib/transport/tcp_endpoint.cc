#include "lib/transport/tcp_endpoint.h"

TCPMessageHandler::TCPMessageHandler(int fd, const Address &other, MessageHandlerFunc msghdl)
    : fd_(fd)
    , handlerFunc_(msghdl)
    , other_(other)
{
    evWatcher_ = new ev_io();
    evWatcher_->data = (void *) this;

    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        TCPMessageHandler *m = (TCPMessageHandler *) (w->data);

        if (m->offset_ == 0) {
            int ret = recv(m->fd_, m->recvBuffer_, sizeof(MessageHeader), 0);
            if (ret > 0 && (uint32_t) ret > sizeof(MessageHeader)) {
                MessageHeader *msgHeader = (MessageHeader *) (void *) (m->recvBuffer_);
                m->offset_ = ret;
                m->remaining_ = msgHeader->msgLen + msgHeader->sigLen;
            } else {
                LOG(WARNING) << "recv error";
                return;
            }
        } else {
            int ret = recv(m->fd_, m->recvBuffer_ + m->offset_, m->remaining_, 0);

            if (ret < 0) {
                LOG(WARNING) << "recv error";
                return;
            }

            m->offset_ += ret;
            m->remaining_ -= ret;

            // Complete message received
            if (m->remaining_ == 0) {
                MessageHeader *msgHeader = (MessageHeader *) (void *) (m->recvBuffer_);

                assert(m->offset_ == sizeof(MessageHeader) + msgHeader->msgLen + msgHeader->sigLen);
                m->handlerFunc_(msgHeader, m->recvBuffer_ + sizeof(MessageHeader), &m->other_);

                // Ready to receive next message
                m->offset_ = 0;
            }
        }
    });

    ev_io_set(evWatcher_, fd_, EV_READ);
}

TCPMessageHandler::~TCPMessageHandler() { delete evWatcher_; }

TCPEndpoint::TCPEndpoint(
    const std::string &ip, const int port, const bool isMasterReceiver, const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
{
    listenFd_ = socket(PF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        LOG(ERROR) << "Receiver Fd fail ";
        return;
    }
    // Set Non-Blocking
    int status = fcntl(listenFd_, F_SETFL, fcntl(listenFd_, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) {
        LOG(ERROR) << " Set NonBlocking Fail";
    }

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    // Bind socket to Address
    int bindRet = bind(listenFd_, (struct sockaddr *) &addr, sizeof(addr));
    if (bindRet != 0) {
        LOG(ERROR) << "bind error\t" << bindRet << "\t port=" << port;
        return;
    }

    // Listen for incoming connections
    if (listen(listenFd_, SOMAXCONN) < 0) {
        LOG(ERROR) << "Listen failed";
        return;
    }

    // Set up an ev_io watcher to handle incoming connections
    ev_io *acceptWatcher = new ev_io();
    acceptWatcher->data = this;
    ev_init(acceptWatcher, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        TCPEndpoint *endpoint = (TCPEndpoint *) w->data;
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);

        int clientFd = accept(endpoint->listenFd_, (struct sockaddr *) &addr, &addrLen);
        if (clientFd < 0) {
            LOG(ERROR) << "Accept failed";
            return;
        }

        // Set client socket to non-blocking
        int status = fcntl(clientFd, F_SETFL, fcntl(clientFd, F_GETFL, 0) | O_NONBLOCK);
        if (status < 0) {
            LOG(ERROR) << "Set NonBlocking Fail on client socket";
        }

        // Setup handler and state
        Address otherAddr(addr);

        endpoint->msgHandlers_.emplace(clientFd, TCPMessageHandler(clientFd, otherAddr, endpoint->handlerFunc_));
        endpoint->addressToSock_.emplace(otherAddr, clientFd);
    });
    ev_io_set(acceptWatcher, listenFd_, EV_READ);
    ev_io_start(evLoop_, acceptWatcher);
}

TCPEndpoint::~TCPEndpoint() {}

int TCPEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (!addressToSock_.contains(dstAddr)) {
        LOG(WARNING) << "Attempting to send to unrecognized address " << dstAddr;
        return -1;
    }
    int fd = addressToSock_.at(dstAddr);

    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    int ret = send(fd, hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    if (ret < 0) {
        VLOG(1) << "\tSend Fail: " << strerror(errno);
        VLOG(1) << "Message size: " << (sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen);
    }

    return ret;
}

bool TCPEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    if (msgHandlers_.size() > 0) {
        LOG(ERROR) << "Attempting to register Message Handler after connections have been made!";
        return false;
    }
    handlerFunc_ = hdl;
    return true;
}
