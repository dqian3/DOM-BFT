#include "lib/crypto/hmac_provider.h"
#include <filesystem>
#include <glog/logging.h>

#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>

using namespace CryptoPP;

HMACProvider::HMACProvider() {}

HMACProvider::~HMACProvider() {}

// Assumes keys are in directory keyDir with names ending in an _n.pub, where n is the id number
bool HMACProvider::loadClientKeysDev(NodeID self, uint32_t nReplicas)
{
    assert(self.first == NodeType::CLIENT);

    for (uint32_t i = 0; i < nReplicas; i++) {
        keys_[{NodeType::REPLICA, i}] = "client" + std::to_string(self.second) + "_replica" + std::to_string(i);
    }

    return true;
}

bool HMACProvider::loadReplicaKeysDev(NodeID self, uint32_t nClients)
{
    assert(self.first == NodeType::REPLICA);

    for (uint32_t i = 0; i < nClients; i++) {
        keys_[{NodeType::CLIENT, i}] = "client" + std::to_string(i) + "_replica" + std::to_string(self.second);
    }
    return true;
}

bool HMACProvider::appendMAC(MessageHeader *hdr, uint32_t bufLen, NodeID dst)
{
#if SKIP_CRYPTO
    return true;
#endif

    byte *data = (byte *) (hdr + 1);
    byte *sig = data + hdr->msgLen;

    if (sizeof(MessageHeader) + hdr->msgLen + HMAC<SHA256>::DIGESTSIZE > bufLen) {
        LOG(ERROR) << "Error signing message, signing would exceed given buffer capacity " << bufLen;
        return false;
    }

    const std::string &key = keys_[dst];

    HMAC<SHA256> hmac(key.data(), key.size());
    hmac.CalculateDigest(sig, data, hdr->msgLen);

    hdr->sigLen = CryptoPP::HMAC<CryptoPP::SHA256>::DIGESTSIZE;

    return true;
}

bool HMACProvider::verify(byte *data, uint32_t dataLen, byte *sig, uint32_t sigLen, NodeID signer)
{
#if SKIP_CRYPTO
    return true;
#endif
    const std::string &key = keys_[signer];

    HMAC<SHA256> verifier(key, key_len);
    bool valid = verifier.VerifyDigest(sig, data, dataLen);
}

bool HMACProvider::verify(MessageHeader *hdr, NodeID signer)
{
    byte *data = (byte *) (hdr + 1);
    return verify(data, hdr->msgLen, data + hdr->msgLen, hdr->sigLen, signer);
}

bool HMACProvider::verify(const std::string &data, const std::string &sig, NodeID signer)
{
    return verify((byte *) data.c_str(), data.size(), (byte *) sig.c_str(), sig.size(), signer);
}