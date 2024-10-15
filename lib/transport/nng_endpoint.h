#ifndef NNG_ENDPOINT_H
#define NNG_ENDPOINT_H

#include "lib/transport/endpoint.h"

#include <unordered_map>
#include <vector>

#include <nng/nng.h>

struct NngMessageHandler {

    MessageHandlerFunc handlerFunc_;

    // Each NngMessageHandler corresponds to a socket and dst addres
    nng_socket sock_;
    Address srcAddr_;
    byte *recvBuffer_;

    ev_io evWatcher_;

    // TODO handle the memory of the recvBuffer (belonging to NngEndpoint better)
    NngMessageHandler(MessageHandlerFunc msghdl, nng_socket s, const Address &otherAddr, byte *recvBuffer);
    ~NngMessageHandler();
};

class NngEndpoint : public Endpoint {
protected:
    // Note these are pointers because if vector get's resized the pointers that libev relies
    // on get messed up (this was a really annoying bug to find)
    std::vector<std::unique_ptr<NngMessageHandler>> handlers_;
    std::vector<nng_socket> socks_;
    std::unordered_map<Address, int> addrToSocketIdx_;   // Map from Address to index of in socks_
    std::unordered_map<int, Address> socketIdxToAddr_;   // Map from index in socks_ to Address, reverse of above

    byte recvBuffer_[NNG_BUFFER_SIZE];

public:
    // Takes a number of addresses
    NngEndpoint(const std::vector<std::pair<Address, Address>> &pairs, bool isMasterReceiver = false);
    virtual ~NngEndpoint();

    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, byte *buf) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif