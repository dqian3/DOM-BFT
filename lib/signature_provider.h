#ifndef SIGNED_UDP_ENDPOINT_H
#define SIGNED_UDP_ENDPOINT_H

#include "lib/common_struct.h"
#include <map>
#include <openssl/evp.h>

// TODO make this also tied to addresses?

class SignatureProvider 
{
protected:
    EVP_PKEY *privKey_;

    // Stores public keys for different types of processes by id
    std::map<std::string, std::map<uint32_t, EVP_PKEY *>> pubKeys_;

public:
    SignatureProvider();
    ~SignatureProvider();

    // Assumes hdr is the start of a message in a buffer.
    // TODO is this bad practice?
    int appendSignature(MessageHeader *hdr, uint32_t bufLen);

    // verify mirros how SignedUDPEndpoint passes it to the handler
    bool verify(MessageHeader *hdr, byte *body, const std::string &pubKeyId);


    bool loadPrivateKey(const std::string& privateKeyPath);

    // Assumes keys are in directory keyDir with names ending in an _n.pub, where n is the id number
    bool loadPublicKeys(const std::string& keyType, const std::string& keyDir) ;
};

#endif