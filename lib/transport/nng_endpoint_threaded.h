#ifndef NNG_ENDPOINT_THREAD_H
#define NNG_ENDPOINT_THREAD_H

#include "lib/transport/nng_endpoint.h"
#include "lib/utils.h"

#include <thread>
#include <unordered_map>
#include <vector>

#include <nng/nng.h>

class NngSendThread {

public:
    NngSendThread(nng_socket sock, const Address &addr);
    ~NngSendThread();
    void run();
    void sendMsg(const byte *msg, size_t len);

private:
    std::thread thread_;

    struct ev_loop *evLoop_;
    ev_async stopWatcher_;
    ev_async sendWatcher_;

    nng_socket sock_;
    Address addr_;   // Just for debugging

    BlockingConcurrentQueue<std::vector<byte>> queue_;

    friend class NngEndpointThreaded;
};

class NngRecvThread {

public:
    NngRecvThread(const std::vector<nng_socket> &socks, const std::unordered_map<int, Address> &sockToAddr,
                  struct ev_loop *parentLoop, ev_async *recvWatcher);
    ~NngRecvThread();
    void run();

    BlockingConcurrentQueue<std::pair<std::vector<byte>, Address>> queue_;

private:
    std::thread thread_;
    bool running_;

    // Event handling for receiving packets
    // This loop runs within the thread
    struct IOWatcherData {
        NngRecvThread *r;
        nng_socket sock;
        Address addr;
    };

    struct ev_loop *evLoop_;
    ev_async stopWatcher_;
    std::vector<ev_io> ioWatchers_;
    std::vector<nng_socket> socks_;
    byte recvBuffer_[NNG_BUFFER_SIZE];

    // Communcation with parent endpoint
    struct ev_loop *parentLoop_;
    ev_async *parentRecvWatcher_;

    friend class NngEndpointThreaded;
};

class NngEndpointThreaded : public NngEndpoint {
protected:
    std::unique_ptr<NngRecvThread> recvThread_;
    std::vector<std::unique_ptr<NngSendThread>> sendThreads_;

    ev_async recvWatcher_;

    MessageHandlerFunc hdlrFunc_;

public:
    NngEndpointThreaded(const std::vector<std::pair<Address, Address>> &pairs, bool isMasterReceiver = false);
    virtual ~NngEndpointThreaded();

    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;

    virtual void LoopBreak() override;
};

#endif