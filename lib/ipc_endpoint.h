#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

#define IPC_BUFFER_SIZE (1024)

class IPCEndpoint : public Endpoint {
 protected:
  /* data */
  struct IPCMsgHandler* msgHandler_;

 public:
  IPCEndpoint(const std::string& ipcAddr, const bool isMasterReceiver);
  ~IPCEndpoint();

  int SendMsgTo(const std::string& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType);

  int SendProtoMsgTo(const std::string& dstAddr, 
                const google::protobuf::Message& msg,
                const char msgType);

  bool RegisterMsgHandler(MessageHandler* msgHdl) override;
  bool UnRegisterMsgHandler(MessageHandler* msgHdl) override;
  bool isMsgHandlerRegistered(MessageHandler* msgHdl) override;
  void UnRegisterAllMsgHandlers() override;
};



#endif