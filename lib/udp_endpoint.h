#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

struct UDPMessageHandler : MessageHandler
{
    byte buffer_[UDP_BUFFER_SIZE];
    UDPMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL);
    ~UDPMessageHandler();
};

class UDPEndpoint : public Endpoint
{
protected:
    /* data */
    struct UDPMessageHandler *msgHandler_;
    bool bound_ = false;

    // Buffer for preparing messages
    // bufInUse_ is just for making sure prepare and send calls are 1 to 1. Not thread safe
    byte buffer_[UDP_BUFFER_SIZE];
    bool bufReady_ = false;

public:
    UDPEndpoint(const std::string &ip, const int port,
                const bool isMasterReceiver = false);
    ~UDPEndpoint();

    // Loads message with header prepended into buffer and sets
    // bufReady to true.
    MessageHeader *PrepareMsg(const byte *msg,
                  u_int32_t msgLen,
                  byte msgType);

    MessageHeader *PrepareProtoMsg(
        const google::protobuf::Message &msg,
        const byte msgType);

    int SendPreparedMsgTo(const Address &dstAddr);


    virtual bool RegisterMsgHandler(MessageHandler *msgHdl) override;

    virtual bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    virtual bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    virtual void UnRegisterAllMsgHandlers() override;
};

#endif