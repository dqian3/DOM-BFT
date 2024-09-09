#ifndef NNG_ENDPOINT_THREAD_H
#define NNG_ENDPOINT_THREAD_H

#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_thread.h"
#include "lib/utils.h"

#include <thread>
#include <unordered_map>
#include <vector>

#include <nng/nng.h>

struct NngThreadMessageHandler {

    MessageHandlerFunc handlerFunc_;

    // Each NngMessageHandler corresponds to a socket and dst addres
    nng_socket sock_;
    Address srcAddr_;
    byte *recvBuffer_;

    std::unique_ptr<ev_io> evWatcher_;

    // TODO handle the memory of the recvBuffer (belonging to NngEndpoint better)
    NngThreadMessageHandler(MessageHandlerFunc msghdl, nng_socket s, const Address &otherAddr, byte *recvBuffer);
    ~NngThreadMessageHandler();
};

class NngSendThread {

public:
    NngSendThread(nng_socket sock, const Address &addr);
    ~NngSendThread();
    void run();
    sendMsgTo(const byte *msg, size_t len);

private:
    std::thread thread_;
    bool running_;

    // TODO do an event loop instead of busy waiting
    // struct ev_loop *evLoop_;
    // ev_async stopWatcher_;
    // ev_async sendWatcher_;

    nng_socket sock_;
    Address addr;   // Just for debugging

    RWQueue<std::vector<byte>> queue_;
}

class NngRecvThread {

public:
    NngRecvThread(std::vector<nng_socket> socks_);
    ~NngRecvThread();
    void run();
    bool registerMsgHandler(MessageHandlerFunc);

private:
    std::thread thread_;
    bool running_;

    ev_async *recv_watcher;
    BlockingRWQueue<std::pair<std::vector<byte>, Address>> queue_;
    // TODO do an event loop instead of busy waiting using these
    // struct ev_loop *evLoop_;
    // ev_async stopWatcher_;
}

class NngEndpointThreaded : public NngEndpoint {
protected:
    NngRecvThread recvThread;
    std::vector<NngSendThread> sendThreads;

    ev_async recvWatcher_;

public:
    NngEndpointThreaded(const std::vector<std::pair<Address, Address>> &pairs, bool isMasterReceiver = false);
    virtual ~NngEndpointThreaded();

    virtual int SendPreparedMsgTo(const Address &dstAddr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif