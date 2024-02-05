#ifndef UDP_ENDPOINT_H
#define UDP_ENDPOINT_H

#include "lib/endpoint.h"

class UDPEndpoint : public Endpoint {
 protected:
  /* data */
  struct UDPMsgHandler* msgHandler_;

 public:
  UDPEndpoint(const std::string& ip, const int port,
                    const bool isMasterReceiver = false);
  ~UDPEndpoint();

  int SendMsgTo(const Address& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType);

  int SendProtoMsgTo(const Address& dstAddr, 
                const google::protobuf::Message& msg,
                const char msgType);

  bool RegisterMsgHandler(MessageHandler* msgHdl) override;
  bool UnRegisterMsgHandler(MessageHandler* msgHdl) override;
  bool isMsgHandlerRegistered(MessageHandler* msgHdl) override;
  void UnRegisterAllMsgHandlers() override;
};



#endif