#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

#include <nng/nng.h>

struct NngMessageHandler : MessageHandler
{
    byte buffer_[UDP_BUFFER_SIZE];
    NngMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL);
    ~NngMessageHandler();
};

class NngEndpoint : public Endpoint
{
protected:
    /* data */
    struct UDPMessageHandler *msgHandler_;
    bool bound_ = false;

    // Buffer for preparing messages
    // bufInUse_ is just for making sure prepare and send calls are 1 to 1. Not thread safe
    byte buffer_[UDP_BUFFER_SIZE];
    bool bufReady_ = false;



    nng_socket s = NNG_SOCKET_INITIALIZER;


public:
    NngEndpoint(const std::string &ip, const int port,
                const bool isMasterReceiver = false);
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