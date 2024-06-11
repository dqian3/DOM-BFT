#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/transport/endpoint.h"

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

public:
    UDPEndpoint(const std::string &ip, const int port,
                const bool isMasterReceiver = false);
    ~UDPEndpoint();
    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr) override;

    void setBufReady(bool bufReady);

    virtual bool RegisterMsgHandler(MessageHandler *msgHdl) override;

    virtual bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    virtual bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    virtual void UnRegisterAllMsgHandlers() override;
};

#endif