#include "lib/transport/tcp_endpoint.h"

TCPMessageHandler::TCPMessageHandler(int fd, const Address &other, MessageHandlerFunc msghdl, byte *buffer)
    : fd_(fd)
    , handlerFunc_(msghdl)
    , other_(other)
    , recvBuffer_(buffer)
{
    evWatcher_ = new ev_io();
    evWatcher_->data = (void *) this;

    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        TCPMessageHandler *m = (TCPMessageHandler *) (w->data);

        LOG(INFO) << "MessageHandler called for message from " << m->other_;

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
                m->handlerFunc_(msgHeader, m->other_);

                // Ready to receive next message
                m->offset_ = 0;
            }
        }
    });

    ev_io_set(evWatcher_, fd_, EV_READ);
}

TCPMessageHandler::~TCPMessageHandler() {}

int non_blocking_socket()
{
    int ret = socket(PF_INET, SOCK_STREAM, 0);
    if (ret < 0) {
        LOG(ERROR) << "socket() failed ";
        exit(1);
    }
    // Set Non-Blocking
    int status = fcntl(ret, F_SETFL, fcntl(ret, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) {
        LOG(ERROR) << " Set NonBlocking Fail";
    }
    return ret;
}

TCPEndpoint::TCPEndpoint(
    const std::string &ip, const int port, const bool isMasterReceiver, const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
    , connectLoop(ev_loop_new())
{
    listenFd_ = non_blocking_socket();

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

        LOG(INFO) << "Accept handler called";

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

        LOG(INFO) << "Accept connection from " << otherAddr;

        endpoint->msgHandlers_.emplace(
            clientFd, TCPMessageHandler(clientFd, otherAddr, endpoint->handlerFunc_, endpoint->recvBuffer_)
        );
    });

    ev_io_set(acceptWatcher, listenFd_, EV_READ);
    ev_io_start(evLoop_, acceptWatcher);
}

TCPEndpoint::~TCPEndpoint() {}

void TCPEndpoint::connectToAddrs(const std::vector<Address> &addrs)
{
    // Connect to sockets while also listening for new connections

    for (const Address &addr : addrs) {
        int fd = non_blocking_socket();

        if (connect(fd, (struct sockaddr *) &addr.addr_, sizeof(sockaddr_in)) < 0) {
            // expect EINProgress because our sockets are non blocking
            if (errno != EINPROGRESS) {
                LOG(ERROR) << "Connection failed " << strerror(errno);
                close(fd);
                exit(1);
            }
        }

        addressToSendSock_.emplace(addr, fd);
    }
}

int TCPEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (!addressToSendSock_.contains(dstAddr)) {
        LOG(WARNING) << "Attempting to send to unrecognized address " << dstAddr;
        return -1;
    }
    int fd = addressToSendSock_.at(dstAddr);

    if (hdr == nullptr) {
        hdr = (MessageHeader *) sendBuffer_;
    }

    int ret = send(fd, hdr, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0);
    if (ret < 0) {
        VLOG(1) << "\tSend Fail: " << strerror(errno);
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
