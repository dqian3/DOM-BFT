#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

struct UDPMessageHandler : MessageHandler
{
    char buffer_[UDP_BUFFER_SIZE];
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

    int SendMsgTo(const Address &dstAddr,
                  const char *msg,
                  u_int32_t msgLen,
                  char msgType);

    int SendProtoMsgTo(const Address &dstAddr,
                       const google::protobuf::Message &msg,
                       const char msgType);

    virtual bool RegisterMsgHandler(MessageHandler *msgHdl) override;

    virtual bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    virtual bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    virtual void UnRegisterAllMsgHandlers() override;
};

#endif