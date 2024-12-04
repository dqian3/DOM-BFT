#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/transport/endpoint.h"

struct UDPMessageHandler {
    byte recvBuffer_[UDP_BUFFER_SIZE];
    MessageHandlerFunc msgHandler_;
    struct ev_io *evWatcher_;
    Address sender_;
    UDPMessageHandler(MessageHandlerFunc msghdl);

    ~UDPMessageHandler();
};

class UDPEndpoint : public Endpoint {
protected:
    /** The socket fd it uses to send/recv messages */
    int fd_;
    /* data */
    std::unique_ptr<UDPMessageHandler> msgHandler_;
    bool bound_ = false;

public:
    UDPEndpoint(
        const std::string &ip, const int port, const bool isMasterReceiver = false,
        const std::optional<Address> &loopbackAddr = std::nullopt
    );
    ~UDPEndpoint();
    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif