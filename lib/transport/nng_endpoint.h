#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/transport/endpoint.h"

#include <nng/nng.h>


struct NngMessageHandler
{
    MessageHandlerFunc msgHandler_;
    void *context_;
    Address sender_;
    ev_io *evWatcher_;

    // The base class MessageHandler has a single 
    NngMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL);
    ~NngMessageHandler();
};

class NngEndpoint : public Endpoint
{
protected:
    /* data */
    struct NngMessageHandler *msgHandler_;

    // Buffer for preparing messages
    // bufInUse_ is just for making sure prepare and send calls are 1 to 1. Not thread safe
    std::vector<nng_socket> socks;

    byte recvBuffer_[NNG_BUFFER_SIZE];

public:
    NngEndpoint(const std::vector<Address> &bindAddrs, const std::vector<Address> &sendAddrs, bool isMasterReceiver);
    ~NngEndpoint();

    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif