#ifndef SIGNED_UDP_ENDPOINT_H
#define SIGNED_UDP_ENDPOINT_H

#include "lib/udp_endpoint.h"
#include <openssl/evp.h>

class SignedUDPEndpoint : public UDPEndpoint
{

protected:
    EVP_PKEY *key_;

public:
    SignedUDPEndpoint(const std::string &ip, const int port, EVP_PKEY *key,
                      const bool isMasterReceiver = false);
    ~SignedUDPEndpoint();

    int SignAndSendMsgTo(const Address &dstAddr,
                         const byte *msg,
                         uint32_t msgLen,
                         byte msgType);

    int SignAndSendProtoMsgTo(const Address &dstAddr,
                            const google::protobuf::Message &msg,
                            byte msgType);

    
    // verify mirros how SignedUDPEndpoint passes it to the handler
    bool verify(MessageHeader *hdr, byte *body, EVP_PKEY *pubkey);
};

#endif