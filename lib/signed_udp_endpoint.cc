#include "lib/signed_udp_endpoint.h"

SignedUDPEndpoint::SignedUDPEndpoint(const std::string& ip, const int port,
                                     const bool isMasterReceiver, EVP_PKEY* key)
    : UDPEndpoint(ip, port, isMasterReceiver)
{
  // TODO setup openSSL context
}

SignedUDPEndpoint::~SignedUDPEndpoint() {}

int SignedUDPEndpoint::SignAndSendMsgTo(const Address& dstAddr,
                const char* msg,
                u_int32_t msgLen,
                char msgType) 
{
  // TODO call SendMsgTo after encapsulating msg in MessageHeader and SignedMessage Header


}

int SignedUDPEndpoint::SignAndSendProtoMsg(const Address& dstAddr,
                const google::protobuf::Message& msg,
                char msgType) 
{
  std::string serializedString = msg.SerializeAsString();
  uint32_t msgLen = serializedString.length();
  if (msgLen > 0) {
    SignAndSendMsgTo(dstAddr, serializedString.c_str(), msgLen, msgType);
  }
  return -1;
}


