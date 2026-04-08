#include "lib/transport/tcp_endpoint.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// =====================================================================
// TcpRecvState
// =====================================================================

TcpRecvState::TcpRecvState(int fd, const Address &addr, size_t bufSize)
    : fd(fd)
    , peerAddr(addr)
    , bufferSize(bufSize)
{
    buffer = (byte *)malloc(bufSize);
    memset(buffer, 0, bufSize);
}

TcpRecvState::~TcpRecvState() { free(buffer); }

int TcpRecvState::recvStep()
{
    // Phase 1: Read header if we don't have it yet
    if (totalNeeded == 0) {
        size_t headerSize = sizeof(MessageHeader);
        while (offset < headerSize) {
            ssize_t n = recv(fd, buffer + offset, headerSize - offset, 0);
            if (n > 0) {
                offset += n;
            } else if (n == 0) {
                return -1;   // Disconnect
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;   // Need more data
                return -1;   // Error
            }
        }
        // Header complete — compute total message size
        MessageHeader *hdr = (MessageHeader *)buffer;
        totalNeeded = headerSize + hdr->msgLen + hdr->sigLen;
        if (totalNeeded > bufferSize) {
            LOG(ERROR) << "Message too large: " << totalNeeded << " > " << bufferSize;
            return -1;
        }
    }

    // Phase 2: Read remaining payload + signature
    while (offset < totalNeeded) {
        ssize_t n = recv(fd, buffer + offset, totalNeeded - offset, 0);
        if (n > 0) {
            offset += n;
        } else if (n == 0) {
            return -1;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }

    return 1;   // Complete message ready
}

void TcpRecvState::reset()
{
    offset = 0;
    totalNeeded = 0;
}

// =====================================================================
// TcpSendConn
// =====================================================================

bool TcpSendConn::sendAll(const byte *data, size_t len)
{
    std::lock_guard<std::mutex> lk(mu);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG(WARNING) << "TCP send failed to " << addr << ": " << strerror(errno);
            return false;
        } else {
            return false;
        }
    }
    return true;
}

// =====================================================================
// TcpEndpoint
// =====================================================================

int TcpEndpoint::createSocket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG(ERROR) << "socket() failed: " << strerror(errno);
        exit(1);
    }
    // Non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    // TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // SO_REUSEADDR
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    return fd;
}

TcpEndpoint::TcpEndpoint(
    const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver,
    const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr)
{
    if (addrPairs.empty()) return;

    // The bind address is pairs[0].first's IP with pairs[0].first's port
    bindAddr_ = addrPairs[0].first;

    // Build address maps (same structure as NNG)
    for (size_t i = 0; i < addrPairs.size(); i++) {
        const Address &connectAddr = addrPairs[i].second;
        addrToIdx_[connectAddr] = i;
        idxToAddr_[i] = connectAddr;
    }

    // Create listen socket on bind address
    listenFd_ = createSocket();
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bindAddr_.port());
    addr.sin_addr.s_addr = inet_addr(bindAddr_.ip().c_str());

    if (bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG(ERROR) << "TCP bind failed on " << bindAddr_ << ": " << strerror(errno);
        exit(1);
    }
    if (listen(listenFd_, SOMAXCONN) < 0) {
        LOG(ERROR) << "TCP listen failed: " << strerror(errno);
        exit(1);
    }

    LOG(INFO) << "TCP endpoint listening on " << bindAddr_;

    // Create send connections (not connected yet)
    sendConns_.resize(addrPairs.size());
    for (size_t i = 0; i < addrPairs.size(); i++) {
        sendConns_[i] = std::make_unique<TcpSendConn>(addrPairs[i].second);
    }
}

TcpEndpoint::~TcpEndpoint()
{
    if (listenFd_ >= 0) close(listenFd_);
    for (auto &conn : sendConns_) {
        if (conn && conn->fd >= 0) close(conn->fd);
    }
    for (auto &state : recvStates_) {
        if (state && state->fd >= 0) close(state->fd);
    }
}

void TcpEndpoint::Connect()
{
    if (connected_) return;

    // We need to simultaneously:
    // 1. Accept incoming connections from peers
    // 2. Connect outgoing to peers
    // Use a temporary event loop for the connection phase

    size_t numPeers = sendConns_.size();
    size_t numAccepted = 0;
    size_t numConnected = 0;

    // Accept incoming connections in a thread
    std::thread acceptThread([&]() {
        while (numAccepted < numPeers) {
            struct sockaddr_in peerAddr{};
            socklen_t addrLen = sizeof(peerAddr);
            int clientFd = accept(listenFd_, (struct sockaddr *)&peerAddr, &addrLen);
            if (clientFd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(10000);   // 10ms
                    continue;
                }
                LOG(ERROR) << "accept failed: " << strerror(errno);
                continue;
            }

            // Set TCP_NODELAY + non-blocking on accepted socket
            int one = 1;
            setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            int flags = fcntl(clientFd, F_GETFL, 0);
            fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

            // Read the peer's identity: the Address they want us to know them as
            // Format: [2B port (network order)][2B ip_len][ip_len bytes of IP string]
            byte portBuf[2];
            size_t got = 0;
            while (got < 2) {
                ssize_t n = recv(clientFd, portBuf + got, 2 - got, 0);
                if (n > 0) got += n;
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
                else break;
            }
            if (got < 2) { LOG(WARNING) << "Failed to read peer identity"; close(clientFd); continue; }

            uint16_t peerListenPort;
            memcpy(&peerListenPort, portBuf, 2);
            peerListenPort = ntohs(peerListenPort);

            // Match to known peer by finding which addrToIdx_ entry has this port
            // Use the IP from the accepted socket's peer address
            std::string peerIpStr = inet_ntoa(peerAddr.sin_addr);
            Address knownAddr(peerIpStr, peerListenPort);

            // If we can't find by (peerIp, port), try matching by port alone (for 0.0.0.0 binds)
            // by checking all known addresses
            bool found = false;
            for (auto &[addr, idx] : addrToIdx_) {
                if (addr.port() == (int)peerListenPort) {
                    knownAddr = addr;
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG(WARNING) << "Unknown peer identity port=" << peerListenPort << " ip=" << peerIpStr;
                close(clientFd);
                continue;
            }

            recvStates_.push_back(std::make_unique<TcpRecvState>(clientFd, knownAddr, SEND_BUFFER_SIZE));
            numAccepted++;

            LOG(INFO) << "TCP accepted connection from " << knownAddr << " (fd=" << clientFd << ")";
        }
    });

    // Connect outgoing to all peers
    for (size_t i = 0; i < numPeers; i++) {
        auto &conn = sendConns_[i];
        const Address &dst = conn->addr;

        while (true) {
            int fd = createSocket();
            struct sockaddr_in dstAddr{};
            dstAddr.sin_family = AF_INET;
            dstAddr.sin_port = htons(dst.port());
            inet_pton(AF_INET, dst.ip().c_str(), &dstAddr.sin_addr);

            // Blocking connect (temporarily make blocking)
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

            int ret = connect(fd, (struct sockaddr *)&dstAddr, sizeof(dstAddr));
            if (ret < 0) {
                close(fd);
                LOG(INFO) << "TCP connect to " << dst << " failed (" << strerror(errno) << "), retrying...";
                usleep(500000);   // 500ms
                continue;
            }

            // Restore non-blocking
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            conn->fd = fd;

            // Send our listen port so the receiver can identify us
            byte portBuf[2];
            uint16_t myPort = htons(bindAddr_.port());
            memcpy(portBuf, &myPort, 2);

            size_t sent = 0;
            while (sent < 2) {
                ssize_t n = ::send(fd, portBuf + sent, 2 - sent, MSG_NOSIGNAL);
                if (n > 0) sent += n;
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                else break;
            }

            numConnected++;
            LOG(INFO) << "TCP connected to " << dst << " (fd=" << fd << ")";
            break;
        }
    }

    acceptThread.join();

    connected_ = true;
    LOG(INFO) << "TCP all " << numPeers << " connections established";
}

void TcpEndpoint::acceptCb(struct ev_loop *loop, ev_io *w, int revents)
{
    // Not used in threaded mode; only for non-threaded fallback
}

void TcpEndpoint::recvCb(struct ev_loop *loop, ev_io *w, int revents)
{
    TcpRecvState *state = (TcpRecvState *)w->data;
    TcpEndpoint *ep = (TcpEndpoint *)((byte *)w->data - offsetof(TcpRecvState, fd));
    // This callback is only for the non-threaded path (not currently used)
}

int TcpEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) hdr = (MessageHeader *)sendBuffer_;

    // Loopback check
    if (loopbackAddr_.has_value() && dstAddr == loopbackAddr_.value()) {
        if (handlerFunc_) {
            byte *body = (byte *)(hdr + 1);
            handlerFunc_(hdr, body, const_cast<Address *>(&dstAddr));
        }
        return 0;
    }

    auto it = addrToIdx_.find(dstAddr);
    if (it == addrToIdx_.end()) {
        LOG(WARNING) << "TCP send to unknown address " << dstAddr;
        return -1;
    }

    size_t totalLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
    if (!sendConns_[it->second]->sendAll((const byte *)hdr, totalLen)) {
        return -1;
    }
    return 0;
}

bool TcpEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    handlerFunc_ = hdl;

    // Register recv watchers on the main event loop for non-threaded mode
    for (auto &state : recvStates_) {
        ev_io watcher;
        watcher.data = state.get();
        ev_init(&watcher, [](struct ev_loop *loop, ev_io *w, int revents) {
            TcpRecvState *s = (TcpRecvState *)w->data;
            // Walk up to find the endpoint... this is hacky for non-threaded mode
            // The threaded variant overrides this method entirely
        });
        ev_io_set(&watcher, state->fd, EV_READ);
        ev_io_start(evLoop_, &watcher);
        recvWatchers_.push_back(watcher);
    }

    return true;
}

void TcpEndpoint::LoopBreak()
{
    Endpoint::LoopBreak();
}

// =====================================================================
// TcpSendThread
// =====================================================================

TcpSendThread::TcpSendThread(TcpSendConn *conn)
    : conn_(conn)
{
    thread_ = std::thread(&TcpSendThread::run, this);
}

TcpSendThread::~TcpSendThread()
{
    stopping = true;
    if (thread_.joinable()) thread_.join();
}

void TcpSendThread::sendMsg(const byte *data, size_t len)
{
    queue_.enqueue(std::vector<byte>(data, data + len));
}

void TcpSendThread::run()
{
    std::vector<byte> msg;
    while (!stopping) {
        if (!queue_.wait_dequeue_timed(msg, 50000)) continue;

        conn_->sendAll(msg.data(), msg.size());

        // Drain any additional queued messages
        while (queue_.try_dequeue(msg)) {
            if (stopping) break;
            conn_->sendAll(msg.data(), msg.size());
        }
    }
}

// =====================================================================
// TcpRecvThread
// =====================================================================

TcpRecvThread::TcpRecvThread(
    std::vector<std::unique_ptr<TcpRecvState>> &states, struct ev_loop *parentLoop, ev_async *parentWatcher
)
    : parentLoop_(parentLoop)
    , parentWatcher_(parentWatcher)
{
    evLoop_ = ev_loop_new();

    watchers_.resize(states.size());
    for (size_t i = 0; i < states.size(); i++) {
        watchers_[i].data = states[i].get();

        ev_init(&watchers_[i], [](struct ev_loop *loop, ev_io *w, int revents) {
            TcpRecvState *s = (TcpRecvState *)w->data;

            while (true) {
                int rc = s->recvStep();
                if (rc == 1) {
                    // Complete message — copy and enqueue
                    MessageHeader *hdr = (MessageHeader *)s->buffer;
                    size_t totalLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;

                    // Find the recv thread via the watcher's loop user data
                    TcpRecvThread *t = (TcpRecvThread *)ev_userdata(loop);
                    t->queue_.enqueue({std::vector<byte>(s->buffer, s->buffer + totalLen), s->peerAddr});
                    ev_async_send(t->parentLoop_, t->parentWatcher_);

                    s->reset();
                    // Try reading next message immediately
                    continue;
                } else if (rc == 0) {
                    break;   // Need more data, return to event loop
                } else {
                    // Error or disconnect
                    LOG(INFO) << "TCP connection from " << s->peerAddr << " closed";
                    ev_io_stop(loop, w);
                    close(s->fd);
                    s->fd = -1;
                    break;
                }
            }
        });

        ev_io_set(&watchers_[i], states[i]->fd, EV_READ);
        ev_io_start(evLoop_, &watchers_[i]);
    }

    // Set user data so recv callbacks can find the thread
    ev_set_userdata(evLoop_, this);

    thread_ = std::thread(&TcpRecvThread::run, this);
}

TcpRecvThread::~TcpRecvThread()
{
    if (thread_.joinable()) {
        ev_async_send(evLoop_, &stopWatcher_);
        thread_.join();
    }
}

void TcpRecvThread::run()
{
    ev_async_init(&stopWatcher_, [](struct ev_loop *loop, ev_async *w, int revents) {
        ev_break(loop, EVBREAK_ALL);
    });
    ev_async_start(evLoop_, &stopWatcher_);
    ev_set_priority(&stopWatcher_, EV_MAXPRI);

    ev_run(evLoop_, 0);
}

// =====================================================================
// TcpEndpointThreaded
// =====================================================================

TcpEndpointThreaded::TcpEndpointThreaded(
    const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver,
    const std::optional<Address> &loopbackAddr
)
    : TcpEndpoint(addrPairs, isMasterReceiver, loopbackAddr)
{
    // Connection is deferred to Connect()
}

TcpEndpointThreaded::~TcpEndpointThreaded() {}

void TcpEndpointThreaded::Connect()
{
    TcpEndpoint::Connect();
    startThreadsIfReady();
}

void TcpEndpointThreaded::startThreadsIfReady()
{
    if (threadsStarted_ || !connected_ || !hdlrFunc_) return;
    threadsStarted_ = true;

    for (auto &conn : sendConns_) {
        sendThreads_.push_back(std::make_unique<TcpSendThread>(conn.get()));
    }
    recvThread_ = std::make_unique<TcpRecvThread>(recvStates_, evLoop_, &recvAsyncWatcher_);
    LOG(INFO) << "TcpEndpointThreaded: started " << sendThreads_.size() << " send threads + recv thread";
}

int TcpEndpointThreaded::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) hdr = (MessageHeader *)sendBuffer_;

    // Loopback
    if (loopbackAddr_.has_value() && dstAddr == loopbackAddr_.value()) {
        byte *msg = (byte *)hdr;
        size_t totalLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
        recvThread_->queue_.enqueue({std::vector<byte>(msg, msg + totalLen), dstAddr});
        ev_async_send(evLoop_, &recvAsyncWatcher_);
        return 0;
    }

    auto it = addrToIdx_.find(dstAddr);
    if (it == addrToIdx_.end()) {
        LOG(WARNING) << "TCP send to unknown address " << dstAddr;
        return -1;
    }

    size_t totalLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
    sendThreads_[it->second]->sendMsg((const byte *)hdr, totalLen);
    return 0;
}

bool TcpEndpointThreaded::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    hdlrFunc_ = hdl;
    recvAsyncWatcher_.data = this;

    auto cb = [](struct ev_loop *loop, ev_async *w, int revents) {
        TcpEndpointThreaded *ep = (TcpEndpointThreaded *)w->data;
        std::pair<std::vector<byte>, Address> item;

        while (ep->recvThread_->queue_.try_dequeue(item)) {
            auto &[msg, addr] = item;
            size_t totalLen = msg.size();
            size_t offset = 0;

            while (totalLen - offset > sizeof(MessageHeader)) {
                byte *msgStart = msg.data() + offset;
                MessageHeader *hdr = (MessageHeader *)msgStart;
                size_t msgLen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;

                if (offset + msgLen <= totalLen) {
                    ep->hdlrFunc_(hdr, msgStart + sizeof(MessageHeader), &addr);
                } else {
                    LOG(WARNING) << "Malformed TCP message";
                }
                offset += msgLen;
            }
        }
    };

    ev_async_init(&recvAsyncWatcher_, cb);
    ev_async_start(evLoop_, &recvAsyncWatcher_);

    // Start threads if connections are established (Connect() called before RegisterMsgHandler)
    startThreadsIfReady();

    return true;
}

void TcpEndpointThreaded::LoopBreak()
{
    LOG(INFO) << "TcpEndpointThreaded::LoopBreak()";

    // Stop send threads
    for (auto &t : sendThreads_) {
        t->stopping = true;
    }
    sendThreads_.clear();   // Destructor joins

    // Stop recv thread
    recvThread_.reset();   // Destructor joins

    ev_break(evLoop_, EVBREAK_ALL);
}
