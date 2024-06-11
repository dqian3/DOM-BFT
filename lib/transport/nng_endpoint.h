#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/transport/endpoint.h"

#include <nng/nng.h>


// Note, this is actually multiple different message handlers
// TODO how to handle multiple handlers on different threads?
struct NngMessageHandler : MessageHandler
{
    // The base class MessageHandler has a single 


    byte buffer_[NNG_BUFFER_SIZE];
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
    byte buffer_[NNG_BUFFER_SIZE];
    bool bufReady_ = false;

    std::vector<nng_socket> socks;


public:
    NngEndpoint(const std::vector<Address> bindAddrs, const std::vector<Address> sendAddrs);
    ~NngEndpoint();

    // Loads message with header prepended into buffer and sets
    // bufReady to true. TODO get some info about buffer size.
    MessageHeader *PrepareMsg(const byte *msg,
                  u_int32_t msgLen,
                  byte msgType);

    MessageHeader *PrepareProtoMsg(
        const google::protobuf::Message &msg,
        const byte msgType);

    // Sends message in buffer
    int SendPreparedMsgTo(const Address &dstAddr, bool reuseBuf=false);
    void setBufReady(bool bufReady);

    virtual bool RegisterMsgHandler(MessageHandler *msgHdl) override;

    virtual bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    virtual bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    virtual void UnRegisterAllMsgHandlers() override;
};

#endif