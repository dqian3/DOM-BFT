#include "lib/transport/tcp_endpoint.h"
#include <list>

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

TCPConnectHelper::TCPConnectHelper(struct ev_loop *loop, Address addr, uint32_t *remaining)
    : connectAddr_(addr)
    , remaining_(remaining)
    , connectWatcher_()
    , retryWatcher_()
{
    // Given an event loop, peridocally attempts to connect to addr.

    // This is done by registering a timer to make the connect call, and then
    // when the connect call is successful, removes its watchers/timers

    // When remaining (a shared counter) hits 0, we can exit the event loop.

    connectWatcher_.data = this;
    ev_init(&connectWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        TCPConnectHelper *helper = reinterpret_cast<TCPConnectHelper *>(w->data);

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        if (so_error == ECONNREFUSED) {
            // Other side is not up yet, retry upon timeout
            close(w->fd);
            ev_timer_again(loop, &helper->retryWatcher_);

            LOG(ERROR) << "Retrying connection for " << helper->connectAddr_ << "\n";

            return;
        } else if (so_error != 0) {
            LOG(ERROR) << "Connection failed: " << strerror(so_error) << "\n";
            exit(1);
        }

        ev_io_stop(loop, w);
        helper->fd = w->fd;

        LOG(INFO) << helper->fd << " -> " << helper->connectAddr_;

        (*helper->remaining_)--;

        if (*helper->remaining_ == 0) {
            LOG(INFO) << "Finished establishing connections!";
            ev_break(loop, EVBREAK_ONE);   // Stop the loop since we are done
        } else {
            LOG(INFO) << *helper->remaining_ << " connections remaining";
        }
    });

    retryWatcher_.data = this;
    ev_init(&retryWatcher_, [](struct ev_loop *loop, struct ev_timer *w, int revents) {
        int fd = non_blocking_socket();
        TCPConnectHelper *helper = reinterpret_cast<TCPConnectHelper *>(w->data);

        if (connect(fd, (struct sockaddr *) &helper->connectAddr_.addr_, sizeof(sockaddr_in)) < 0) {
            // expect EINProgress because our sockets are non blocking
            if (errno != EINPROGRESS) {
                LOG(ERROR) << "Connection failed " << strerror(errno);
                close(fd);
                exit(1);
            }
        }

        ev_timer_stop(loop, w);
        ev_io_set(&helper->connectWatcher_, fd, EV_WRITE);
        ev_io_start(loop, &helper->connectWatcher_);
    });

    // Try connecting after 1 second
    retryWatcher_.repeat = 1;
    ev_timer_again(loop, &retryWatcher_);
}

TCPEndpoint::TCPEndpoint(
    const std::string &ip, const int port, const bool isMasterReceiver, const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
    , evConnectLoop_(ev_loop_new())
    , acceptWatcher_()
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
        exit(1);
        return;
    }

    // Listen for incoming connections
    if (listen(listenFd_, SOMAXCONN) < 0) {
        LOG(ERROR) << "Listen failed";
        return;
    }

    acceptWatcher_.data = this;

    ev_init(&acceptWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
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

    ev_io_set(&acceptWatcher_, listenFd_, EV_READ);
    // ev_io_start(evLoop_, &acceptWatcher_);
}

TCPEndpoint::~TCPEndpoint()
{
    close(listenFd_);
    for (auto &entry : msgHandlers_) {
        close(entry.first);
    }

    for (auto &entry : addressToSendSock_) {
        close(entry.second);
    }
}

void TCPEndpoint::connectToAddrs(const std::vector<Address> &addrs)
{
    // Connect to sockets while also listening for new connections
    // Blocks until all sockets are connected. Should only be called once
    if (connected_) {
        LOG(ERROR) << "Redundant connectToAddrs call! Exiting";
        exit(1);
    }

    if (addrs.size() == 0) {
        connected_ = true;
        return;
    }

    uint32_t numConnRemaining = addrs.size();

    // TODO, we have to use a vector of pointers here because the TCPConnect Helpers don't copy nicely...
    std::vector<std::unique_ptr<TCPConnectHelper>> connectHelpers;

    for (size_t i = 0; i < addrs.size(); i++) {
        const Address &addr = addrs[i];
        // Setup helper to establish for connection event
        connectHelpers.emplace_back(std::make_unique<TCPConnectHelper>(evConnectLoop_, addr, &numConnRemaining));
    }
    // Start a event loop to wait for all connections to be established while also accepting our own conns
    ev_io_start(evConnectLoop_, &acceptWatcher_);
    // ev_io_stop(evLoop_, &acceptWatcher_);

    LOG(INFO) << "Starting connection loop";
    ev_run(evConnectLoop_, 0);
    LOG(INFO) << "Done connection loop";

    // Register addresses to their sockets
    for (auto &h : connectHelpers) {
        addressToSendSock_[h->connectAddr_] = h->fd;
        // LOG(INFO) << h.fd << " -> " << h.connectAddr_;
    }

    // Continue to accept connections in main loop
    ev_io_stop(evConnectLoop_, &acceptWatcher_);
    ev_io_start(evLoop_, &acceptWatcher_);
    connected_ = true;
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
        LOG(WARNING) << "Note, registering Message Handler after some connections have been made!";

        for (auto &hdlr : msgHandlers_) {
            hdlr.second.handlerFunc_ = hdl;
        }
    }
    handlerFunc_ = hdl;
    return true;
}

void TCPEndpoint::LoopRun()
{
    if (!connected_) {
        LOG(ERROR) << "Attempting to start event loop without connections... exiting";
        exit(1);
    }

    Endpoint::LoopRun();
}