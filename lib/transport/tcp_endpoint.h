#ifndef TCP_ENDPOINT_H
#define TCP_ENDPOINT_H

#include "lib/transport/endpoint.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <netinet/tcp.h>

// Per-connection receive state: reassembles MessageHeader-framed messages from TCP stream
struct TcpRecvState {
    int fd;
    Address peerAddr;

    // Reassembly buffer
    byte *buffer;
    size_t bufferSize;
    size_t offset = 0;       // Bytes received so far for current message
    size_t totalNeeded = 0;  // Total bytes needed (0 = still reading header)

    TcpRecvState(int fd, const Address &addr, size_t bufSize);
    ~TcpRecvState();

    // Returns true if a complete message is available (header at buffer[0])
    // Handles partial reads, EAGAIN, disconnects.
    // Returns: 1 = complete message ready, 0 = need more data, -1 = error/disconnect
    int recvStep();

    // Reset state for next message after processing
    void reset();
};

// Per-peer send connection with mutex (mirrors dummy-cpp's PeerConn)
struct TcpSendConn {
    int fd = -1;
    Address addr;
    std::mutex mu;

    TcpSendConn() = default;
    explicit TcpSendConn(const Address &a) : addr(a) {}

    // Send a complete message (header + payload + sig). Handles partial writes.
    bool sendAll(const byte *data, size_t len);
};

class TcpEndpoint : public Endpoint {
protected:
    int listenFd_ = -1;
    Address bindAddr_;

    // Peer address mapping (like NNG's addrToSocketIdx)
    std::unordered_map<Address, int> addrToIdx_;
    std::unordered_map<int, Address> idxToAddr_;

    // Send connections (one per peer, indexed same as addrToIdx_)
    std::vector<std::unique_ptr<TcpSendConn>> sendConns_;

    // Receive state (one per accepted connection)
    std::vector<std::unique_ptr<TcpRecvState>> recvStates_;
    std::vector<ev_io> recvWatchers_;

    // Accept watcher
    ev_io acceptWatcher_;

    MessageHandlerFunc handlerFunc_;
    bool connected_ = false;

    // Helper to create a non-blocking TCP socket with TCP_NODELAY + SO_REUSEADDR
    static int createSocket();

    // Establish outgoing connections to all peers. Blocks until all connected.
    void connectToPeers(const std::vector<Address> &peerAddrs);

    // Accept handler
    static void acceptCb(struct ev_loop *loop, ev_io *w, int revents);

    // Recv handler
    static void recvCb(struct ev_loop *loop, ev_io *w, int revents);

public:
    TcpEndpoint(
        const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver = false,
        const std::optional<Address> &loopbackAddr = std::nullopt
    );
    virtual ~TcpEndpoint();

    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc hdl) override;
    virtual void Connect() override;
    virtual void LoopBreak() override;
};

// Threaded variant: dedicated send threads per peer + recv thread
class TcpSendThread {
public:
    TcpSendThread(TcpSendConn *conn);
    ~TcpSendThread();

    void sendMsg(const byte *data, size_t len);

    std::atomic<bool> stopping{false};

private:
    void run();

    TcpSendConn *conn_;
    std::thread thread_;
    BlockingConcurrentQueue<std::vector<byte>> queue_;
};

class TcpRecvThread {
public:
    TcpRecvThread(
        std::vector<std::unique_ptr<TcpRecvState>> &states, struct ev_loop *parentLoop, ev_async *parentWatcher
    );
    ~TcpRecvThread();

    BlockingConcurrentQueue<std::pair<std::vector<byte>, Address>> queue_;

private:
    void run();

    std::thread thread_;
    struct ev_loop *evLoop_;
    ev_async stopWatcher_;
    std::vector<ev_io> watchers_;

    struct ev_loop *parentLoop_;
    ev_async *parentWatcher_;
};

class TcpEndpointThreaded : public TcpEndpoint {
protected:
    std::vector<std::unique_ptr<TcpSendThread>> sendThreads_;
    std::unique_ptr<TcpRecvThread> recvThread_;
    ev_async recvAsyncWatcher_;
    MessageHandlerFunc hdlrFunc_;

public:
    TcpEndpointThreaded(
        const std::vector<std::pair<Address, Address>> &addrPairs, bool isMasterReceiver = false,
        const std::optional<Address> &loopbackAddr = std::nullopt
    );
    virtual ~TcpEndpointThreaded();

    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc hdl) override;
    virtual void Connect() override;
    virtual void LoopBreak() override;

private:
    bool threadsStarted_ = false;
    void startThreadsIfReady();
};

#endif
