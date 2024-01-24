#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

class IPCEndpoint : public Endpoint {
 protected:
  /* data */
  struct IPCMsgHandler* msgHandler_;

 public:
  IPCEndpoint(const std::string& ipc_addr, const bool isMasterReceiver);
  ~IPCEndpoint();

  int SendMsgTo(const Address& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType) override;

  int SendProtoMsgTo(const Address& dstAddr, 
                const google::protobuf::Message& msg,
                const char msgType);

  bool RegisterMsgHandler(MessageHandler* msgHdl) override;
  bool UnRegisterMsgHandler(MessageHandler* msgHdl) override;
  bool isMsgHandlerRegistered(MessageHandler* msgHdl) override;
  void UnRegisterAllMsgHandlers() override;
};



#endif