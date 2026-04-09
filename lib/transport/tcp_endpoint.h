#ifndef TCP_ENDPOINT_H
#define TCP_ENDPOINT_H

#include "lib/transport/endpoint.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <netinet/tcp.h>

// Per-peer bidirectional TCP connection.
// One fd used for both send and recv. Send is mutex-protected.
struct TcpPeerConn {
    int sendFd = -1;   // Outgoing (dialed) fd — for sending
    int recvFd = -1;   // Incoming (accepted) fd — for receiving
    Address peerAddr;
    std::mutex sendMu;

    // Recv reassembly state
    byte *recvBuf;
    size_t recvBufSize;
    size_t recvOffset = 0;
    size_t recvNeeded = 0;

    TcpPeerConn(const Address &addr, size_t bufSize);
    ~TcpPeerConn();

    bool sendAll(const byte *data, size_t len);
    int recvStep();  // 1=complete msg, 0=need more, -1=error
    void recvReset();
};

class TcpSendThread {
public:
    TcpSendThread(TcpPeerConn *conn);
    ~TcpSendThread();
    void sendMsg(const byte *data, size_t len);
    std::atomic<bool> stopping{false};
private:
    void run();
    TcpPeerConn *conn_;
    std::thread thread_;
    BlockingConcurrentQueue<std::vector<byte>> queue_;
};

class TcpRecvThread {
public:
    TcpRecvThread(std::vector<TcpPeerConn *> conns, struct ev_loop *parentLoop, ev_async *parentWatcher);
    ~TcpRecvThread();
    BlockingConcurrentQueue<std::pair<std::vector<byte>, Address>> queue_;
private:
    void run();
    std::vector<TcpPeerConn *> conns_;
    std::thread thread_;
    struct ev_loop *evLoop_ = nullptr;
    ev_async stopWatcher_;
    std::vector<ev_io> watchers_;
    struct ev_loop *parentLoop_;
    ev_async *parentWatcher_;
};

class TcpEndpointThreaded : public Endpoint {
protected:
    std::vector<std::pair<Address, Address>> pairs_;

    // One connection per pair, used bidirectionally
    std::vector<std::unique_ptr<TcpPeerConn>> conns_;

    // Address -> index
    std::unordered_map<Address, int> addrToIdx_;

    // Threads
    std::vector<std::unique_ptr<TcpSendThread>> sendThreads_;
    std::unique_ptr<TcpRecvThread> recvThread_;
    ev_async recvAsyncWatcher_;
    MessageHandlerFunc hdlrFunc_;

    bool connected_ = false;
    bool threadsStarted_ = false;

    static int createSocket();
    void startThreadsIfReady();

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
};

#endif
