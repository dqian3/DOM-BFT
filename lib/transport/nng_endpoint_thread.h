#ifndef NNG_ENDPOINT_THREAD_H
#define NNG_ENDPOINT_THREAD_H

#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_thread.h"

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

class NngEndpointThread : public NngEndpoint {
protected:
public:
    NngEndpointThread(const std::vector<std::pair<Address, Address>> &pairs, bool isMasterReceiver = false);
    ~NngEndpointThread();

    virtual int SendPreparedMsgTo(const Address &dstAddr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif