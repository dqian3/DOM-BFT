#ifndef SIGNED_UDP_ENDPOINT_H
#define SIGNED_UDP_ENDPOINT_H

#include "lib/common.h"
#include <map>

#include <cryptopp/xed25519.h>

// TODO make this also tied to addresses?

class SignatureProvider {
protected:
    CryptoPP::ed25519PrivateKey privKey_;

    // Stores public keys for different types of processes by id
    std::map<std::string, std::map<uint32_t, CryptoPP::ed25519PublicKey>> pubKeys_;

public:
    SignatureProvider();
    ~SignatureProvider();

    // Assumes hdr is the start of a message in a buffer.
    // TODO is this bad practice?
    bool appendSignature(MessageHeader *hdr, uint32_t bufLen);

    bool verify(byte *data, uint32_t dataLen, byte *sig, uint32_t sigLen, const std::string &pubKeyType, int pubKeyId);
    bool verify(MessageHeader *hdr, const std::string &pubKeyType, int pubKeyId);
    // get signature as a byte vector (to be stored elsewhere) after receiving a message.
    std::vector<byte> getSignature(MessageHeader *hdr);

    bool loadPrivateKey(const std::string &privateKeyPath);

    // Assumes keys are in directory keyDir with names ending in an _n.pub, where n is the id number
    bool loadPublicKeys(const std::string &keyType, const std::string &keyDir);
};

#endif