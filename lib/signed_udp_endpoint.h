#ifndef SIGNED_UDP_ENDPOINT_H
#define SIGNED_UDP_ENDPOINT_H

#include "lib/udp_endpoint.h"
#include <openssl/evp.h>

class SignedUDPEndpoint : public UDPEndpoint {

 public:
  SignedUDPEndpoint(const std::string& ip = "", const int port = -1,
                    const bool isMasterReceiver = false, EVP_PKEY *key);
  ~SignedUDPEndpoint();

  int SignAndSendMsgTo(const Address& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType);

  int SignAndSendProtoMsg(const Address& dstAddr,
          const google::protobuf::Message& msg,
          char msgType);
};


#endif