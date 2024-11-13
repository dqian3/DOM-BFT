#ifndef TCP_ENDPOINT_H
#define TCP_ENDPOINT_H

#include "lib/transport/endpoint.h"

struct TCPMessageHandler {

    int fd_;
    MessageHandlerFunc handlerFunc_;
    Address other_;
    byte *recvBuffer_;
    struct ev_io *evWatcher_;

    // Offset in reading current message
    uint32_t offset_ = 0;
    uint32_t remaining_ = 0;

    TCPMessageHandler(int fd, const Address &other, MessageHandlerFunc msghdl, byte *buffer);

    ~TCPMessageHandler();
};

class TCPEndpoint : public Endpoint {
protected:
    /** The socket fd it uses to listen for connections */
    int listenFd_;

    byte recvBuffer_[TCP_BUFFER_SIZE];
    MessageHandlerFunc handlerFunc_;
    std::unordered_map<int, TCPMessageHandler> msgHandlers_;
    std::unordered_map<Address, int> addressToSendSock_;

public:
    TCPEndpoint(
        const std::string &ip, const int port, const bool isMasterReceiver = false,
        const std::optional<Address> &loopbackAddr = std::nullopt
    );
    ~TCPEndpoint();

    void connectToAddrs(const std::vector<Address> &addrs);

    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;
    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif