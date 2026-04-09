#include "lib/transport/tcp_endpoint.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// =====================================================================
// TcpPeerConn — one bidirectional TCP connection per peer
// =====================================================================

TcpPeerConn::TcpPeerConn(const Address &addr, size_t bufSize)
    : peerAddr(addr), recvBufSize(bufSize)
{
    recvBuf = (byte *)malloc(bufSize);
}

TcpPeerConn::~TcpPeerConn()
{
    free(recvBuf);
    if (sendFd >= 0) close(sendFd);
    if (recvFd >= 0 && recvFd != sendFd) close(recvFd);
}

bool TcpPeerConn::sendAll(const byte *data, size_t len)
{
    std::lock_guard<std::mutex> lk(sendMu);
    if (sendFd < 0) return false;
    int fd = sendFd;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += n; }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        else { return false; }
    }
    return true;
}

int TcpPeerConn::recvStep()
{
    if (recvFd < 0) return -1;
    int fd = recvFd;

    // Phase 1: read header
    if (recvNeeded == 0) {
        size_t hdrSize = sizeof(MessageHeader);
        while (recvOffset < hdrSize) {
            ssize_t n = recv(fd, recvBuf + recvOffset, hdrSize - recvOffset, 0);
            if (n > 0) { recvOffset += n; }
            else if (n == 0) { return -1; }
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
        }
        MessageHeader *hdr = (MessageHeader *)recvBuf;
        recvNeeded = hdrSize + hdr->msgLen + hdr->sigLen;
        if (recvNeeded > recvBufSize) { return -1; }
    }

    // Phase 2: read payload + sig
    while (recvOffset < recvNeeded) {
        ssize_t n = recv(fd, recvBuf + recvOffset, recvNeeded - recvOffset, 0);
        if (n > 0) { recvOffset += n; }
        else if (n == 0) { return -1; }
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }
    return 1;
}

void TcpPeerConn::recvReset() { recvOffset = 0; recvNeeded = 0; }

// =====================================================================
// TcpSendThread
// =====================================================================

TcpSendThread::TcpSendThread(TcpPeerConn *conn) : conn_(conn)
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
    uint64_t sent = 0;
    while (!stopping) {
        if (!queue_.wait_dequeue_timed(msg, 50000)) continue;
        if (!conn_->sendAll(msg.data(), msg.size())) {
            LOG(WARNING) << "TCP sendAll failed to " << conn_->peerAddr << " (fd=" << conn_->sendFd << ")";
        } else {
            sent++;
        }
        while (!stopping && queue_.try_dequeue(msg)) {
            if (!conn_->sendAll(msg.data(), msg.size())) {
                LOG(WARNING) << "TCP sendAll failed to " << conn_->peerAddr;
            } else {
                sent++;
            }
        }
    }
    VLOG(1) << "TCP send thread to " << conn_->peerAddr << " exiting, sent " << sent << " messages";
}

// =====================================================================
// TcpRecvThread — registers watchers inside its own thread
// =====================================================================

TcpRecvThread::TcpRecvThread(
    std::vector<TcpPeerConn *> conns, struct ev_loop *parentLoop, ev_async *parentWatcher
)
    : conns_(std::move(conns)), parentLoop_(parentLoop), parentWatcher_(parentWatcher)
{
    thread_ = std::thread(&TcpRecvThread::run, this);
}

TcpRecvThread::~TcpRecvThread()
{
    if (evLoop_) ev_async_send(evLoop_, &stopWatcher_);
    if (thread_.joinable()) thread_.join();
}

void TcpRecvThread::run()
{
    evLoop_ = ev_loop_new();
    ev_set_userdata(evLoop_, this);

    // Stop watcher
    ev_async_init(&stopWatcher_, [](struct ev_loop *loop, ev_async *, int) {
        ev_break(loop, EVBREAK_ALL);
    });
    ev_async_start(evLoop_, &stopWatcher_);
    ev_set_priority(&stopWatcher_, EV_MAXPRI);

    // IO watchers — created here inside the thread that will run ev_run
    watchers_.resize(conns_.size());
    for (size_t i = 0; i < conns_.size(); i++) {
        if (conns_[i]->recvFd < 0) continue;
        watchers_[i].data = conns_[i];
        ev_init(&watchers_[i], [](struct ev_loop *loop, ev_io *w, int revents) {
            TcpPeerConn *c = (TcpPeerConn *)w->data;
            TcpRecvThread *t = (TcpRecvThread *)ev_userdata(loop);
            while (true) {
                int rc = c->recvStep();
                if (rc == 1) {
                    MessageHeader *hdr = (MessageHeader *)c->recvBuf;
                    size_t len = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
                    t->queue_.enqueue({std::vector<byte>(c->recvBuf, c->recvBuf + len), c->peerAddr});
                    ev_async_send(t->parentLoop_, t->parentWatcher_);
                    c->recvReset();
                } else if (rc == 0) {
                    break;
                } else {
                    ev_io_stop(loop, w);
                    break;
                }
            }
        });
        ev_io_set(&watchers_[i], conns_[i]->recvFd, EV_READ);
        ev_io_start(evLoop_, &watchers_[i]);
    }

    LOG(INFO) << "TCP recv thread running with " << conns_.size() << " connections";
    ev_run(evLoop_, 0);
}

// =====================================================================
// Helpers
// =====================================================================

int TcpEndpointThreaded::createSocket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG(ERROR) << "socket() failed"; exit(1); }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    return fd;
}

// =====================================================================
// TcpEndpointThreaded
// =====================================================================

TcpEndpointThreaded::TcpEndpointThreaded(
    const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver,
    const std::optional<Address> &loopbackAddr
)
    : Endpoint(isMasterReceiver, loopbackAddr), pairs_(addrPairs)
{
    for (size_t i = 0; i < pairs_.size(); i++) {
        addrToIdx_[pairs_[i].second] = i;
        conns_.push_back(std::make_unique<TcpPeerConn>(pairs_[i].second, SEND_BUFFER_SIZE));
    }
    LOG(INFO) << "TCP endpoint created with " << pairs_.size() << " pairs";
}

TcpEndpointThreaded::~TcpEndpointThreaded() {}

void TcpEndpointThreaded::Connect()
{
    if (connected_) return;

    // For each pair, we need ONE fd. Either we dial the peer or the peer dials us.
    // Strategy: both sides try to connect AND listen. Whichever succeeds first wins.
    // The "lower" address (by ip:port string) dials, the "higher" accepts.
    // This avoids duplicate connections.

    size_t n = pairs_.size();

    // Create listen sockets for all pairs
    std::vector<int> listenFds(n, -1);
    for (size_t i = 0; i < n; i++) {
        int fd = createSocket();
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(pairs_[i].first.port());
        addr.sin_addr.s_addr = inet_addr(pairs_[i].first.ip().c_str());
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG(ERROR) << "TCP bind " << pairs_[i].first << ": " << strerror(errno);
            exit(1);
        }
        listen(fd, 4);
        // Non-blocking for polling
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        listenFds[i] = fd;
    }

    // For each pair: dial .second (outgoing) AND accept on .first (incoming).
    // The dialed fd is used for SENDING, the accepted fd is used for RECEIVING.
    // This matches NNG's model where each pair has a bidirectional channel.

    // recvFds[i] will hold the accepted fd for pair i (for receiving)
    std::vector<int> recvFds(n, -1);

    // Accept thread: poll all listen sockets, accept one connection each
    std::atomic<bool> acceptDone{false};
    std::thread acceptThread([&]() {
        while (!acceptDone.load()) {
            for (size_t i = 0; i < n; i++) {
                if (listenFds[i] < 0) continue;
                struct sockaddr_in paddr{};
                socklen_t len = sizeof(paddr);
                int fd = accept(listenFds[i], (struct sockaddr *)&paddr, &len);
                if (fd < 0) continue;
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                recvFds[i] = fd;
                close(listenFds[i]);
                listenFds[i] = -1;
                LOG(INFO) << "TCP [" << i << "] accepted from " << pairs_[i].second;
            }
            if (!acceptDone.load()) usleep(5000);
        }
    });

    // Connect threads: dial .second for each pair (for sending)
    std::vector<std::thread> dialThreads;
    for (size_t i = 0; i < n; i++) {
        dialThreads.emplace_back([this, i]() {
            const Address &dst = pairs_[i].second;

            // Self-pair: skip
            if (pairs_[i].first == dst) {
                LOG(INFO) << "TCP [" << i << "] self-pair, skipping";
                return;
            }

            for (int attempt = 0; attempt < 120; attempt++) {
                int fd = createSocket();
                struct sockaddr_in dstAddr{};
                dstAddr.sin_family = AF_INET;
                dstAddr.sin_port = htons(dst.port());
                inet_pton(AF_INET, dst.ip().c_str(), &dstAddr.sin_addr);

                if (connect(fd, (struct sockaddr *)&dstAddr, sizeof(dstAddr)) == 0) {
                    int flags = fcntl(fd, F_GETFL, 0);
                    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                    conns_[i]->sendFd = fd;
                    LOG(INFO) << "TCP [" << i << "] dialed " << dst << " (fd=" << fd << ")";
                    return;
                }
                close(fd);
                usleep(100000);
            }
            LOG(WARNING) << "TCP [" << i << "] failed to connect to " << dst;
        });
    }

    for (auto &t : dialThreads) t.join();

    // Give accepts a moment to finish, then stop
    usleep(500000);
    acceptDone = true;
    acceptThread.join();

    // Store recv fds
    for (size_t i = 0; i < n; i++) {
        conns_[i]->recvFd = recvFds[i];
    }

    // Close remaining listen sockets
    for (int &fd : listenFds) {
        if (fd >= 0) { close(fd); fd = -1; }
    }

    connected_ = true;
    size_t nSend = 0, nRecv = 0;
    for (auto &c : conns_) {
        if (c->sendFd >= 0) nSend++;
        if (c->recvFd >= 0) nRecv++;
    }
    LOG(INFO) << "TCP connections: " << nSend << " send, " << nRecv << " recv";
    startThreadsIfReady();
}

void TcpEndpointThreaded::startThreadsIfReady()
{
    if (threadsStarted_ || !connected_ || !hdlrFunc_) return;
    threadsStarted_ = true;

    std::vector<TcpPeerConn *> connPtrs;
    for (auto &c : conns_) {
        sendThreads_.push_back(std::make_unique<TcpSendThread>(c.get()));
        connPtrs.push_back(c.get());
    }
    recvThread_ = std::make_unique<TcpRecvThread>(connPtrs, evLoop_, &recvAsyncWatcher_);
    LOG(INFO) << "TCP started " << sendThreads_.size() << " send threads + recv thread";
}

int TcpEndpointThreaded::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr)
{
    if (hdr == nullptr) hdr = (MessageHeader *)sendBuffer_;

    if (loopbackAddr_.has_value() && dstAddr == loopbackAddr_.value()) {
        byte *msg = (byte *)hdr;
        size_t len = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
        if (recvThread_) {
            recvThread_->queue_.enqueue({std::vector<byte>(msg, msg + len), dstAddr});
            ev_async_send(evLoop_, &recvAsyncWatcher_);
        }
        return 0;
    }

    auto it = addrToIdx_.find(dstAddr);
    if (it == addrToIdx_.end()) {
        LOG(WARNING) << "TCP send to unknown " << dstAddr;
        return -1;
    }

    size_t len = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
    sendThreads_[it->second]->sendMsg((const byte *)hdr, len);
    return 0;
}

bool TcpEndpointThreaded::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    hdlrFunc_ = hdl;
    recvAsyncWatcher_.data = this;

    auto recvCb = [](struct ev_loop *loop, ev_async *w, int revents) {
        TcpEndpointThreaded *ep = (TcpEndpointThreaded *)w->data;
        std::pair<std::vector<byte>, Address> item;
        while (ep->recvThread_ && ep->recvThread_->queue_.try_dequeue(item)) {
            auto &[msg, addr] = item;
            size_t total = msg.size();
            size_t off = 0;
            while (total - off > sizeof(MessageHeader)) {
                byte *start = msg.data() + off;
                MessageHeader *hdr = (MessageHeader *)start;
                size_t mlen = sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen;
                if (off + mlen <= total)
                    ep->hdlrFunc_(hdr, start + sizeof(MessageHeader), &addr);
                off += mlen;
            }
        }
    };
    ev_async_init(&recvAsyncWatcher_, recvCb);
    ev_async_start(evLoop_, &recvAsyncWatcher_);

    startThreadsIfReady();
    return true;
}

void TcpEndpointThreaded::LoopBreak()
{
    for (auto &t : sendThreads_) t->stopping = true;
    sendThreads_.clear();
    recvThread_.reset();
    ev_break(evLoop_, EVBREAK_ALL);
}
