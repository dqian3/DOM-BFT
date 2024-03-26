#ifndef IPC_ENDPOINT_H 
#define IPC_ENDPOINT_H

#include "lib/endpoint.h"

struct IPCMessageHandler : MessageHandler
{
    char buffer_[UDP_BUFFER_SIZE];
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
                  const char *msg,
                  u_int32_t msgLen,
                  char msgType);

    int SendProtoMsgTo(const std::string &dstAddr,
                       const google::protobuf::Message &msg,
                       const char msgType);

    bool RegisterMsgHandler(MessageHandler *msgHdl) override;
    bool UnRegisterMsgHandler(MessageHandler *msgHdl) override;
    bool isMsgHandlerRegistered(MessageHandler *msgHdl) override;
    void UnRegisterAllMsgHandlers() override;
};

#endif