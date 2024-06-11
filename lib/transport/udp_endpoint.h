#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/transport/endpoint.h"

struct UDPMessageHandler 
{

    byte buffer_[UDP_BUFFER_SIZE];
    MessageHandlerFunc msgHandler_;
    void *context_;
    Address sender_;
    struct ev_io *evWatcher_;
    UDPMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL)
        : msgHandler_(msghdl), context_(ctx)
    {
        evWatcher_ = new ev_io();
        evWatcher_->data = (void *)this;
    }
 
    UDPMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL);
    ~UDPMessageHandler();
};

class UDPEndpoint : public Endpoint
{
protected:
    /* data */
    std::unique_ptr<UDPMessageHandler> msgHandler_;
    bool bound_ = false;

public:
    UDPEndpoint(const std::string &ip, const int port,
                const bool isMasterReceiver = false);
    ~UDPEndpoint();
    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif