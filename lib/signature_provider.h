#ifndef SIGNED_UDP_ENDPOINT_H
#define SIGNED_UDP_ENDPOINT_H

#include "lib/common_struct.h"
#include <openssl/evp.h>

class SignatureProvider 
{

protected:
    EVP_PKEY *privKey_;

    // TODO id to public key maps

public:
    SignatureProvider(EVP_PKEY *privateKey);
    ~SignatureProvider();

    // Assumes hdr is the start of a message in a buffer.
    // TODO is this bad practice?
    int appendSignature(MessageHeader *hdr, uint32_t bufLen);

    // verify mirros how SignedUDPEndpoint passes it to the handler
    bool verify(MessageHeader *hdr, byte *body, EVP_PKEY *pubkey);
};

#endif