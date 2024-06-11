#ifndef IPC_ENDPOINT_H 
#define IPC_ENDPOINT_H

#include "lib/endpoint.h"

struct IPCMessageHandler : MessageHandler
{
    byte buffer_[IPC_BUFFER_SIZE];
    IPCMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL);
    ~IPCMessageHandler();
};

class IPCEndpoint : public Endpoint
{
protected:
    /* data */
    struct IPCMessageHandler *msgHandler_;

public:
    IPCEndpoint(const std::string &ipcAddr, const bool isMasterReceiver);
    ~IPCEndpoint();

    int SendMsgTo(const std::string &dstAddr,
                  const byte *msg,
                  u_int32_t msgLen,
                  byte msgType);

    int SendProtoMsgTo(const std::string &dstAddr,
                       const google::protobuf::Message &msg,
                       const byte msgType);

    bool RegisterMsgHandler(MessageHandler *msgHdl) override;
    bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    void UnRegisterAllMsgHandlers() override;
};

#endif