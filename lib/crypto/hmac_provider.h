#ifndef HMAC_PROVIDER_H
#define HMAC_PROVIDER_H

#include "lib/common.h"
#include <map>

#include <cryptopp/xed25519.h>

// TODO make this also tied to addresses?

class HMACProvider {
protected:
    // Stores public keys for different types of processes by id
    std::map<NodeID, std::string> keys_;

public:
    HMACProvider();
    ~HMACProvider();

    // Assumes hdr is the start of a message in a buffer.
    // TODO is this bad practice?
    bool appendMAC(MessageHeader *hdr, uint32_t bufLen, NodeID dst);

    bool verify(byte *data, uint32_t dataLen, byte *sig, uint32_t sigLen, NodeID signer);
    bool verify(MessageHeader *hdr, NodeID signer);
    bool verify(const std::string &data, const std::string &sig, NodeID signer);

    // Load some predetermined keys for development
    bool loadClientKeysDev(NodeID self, uint32_t nReplicas);
    bool loadReplicaKeysDev(NodeID self, uint32_t nClients);

    // TODO implement key exchange or predistributed keys
};

#endif