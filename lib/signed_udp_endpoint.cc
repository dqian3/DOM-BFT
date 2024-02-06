#include "lib/signed_udp_endpoint.h"

#define MAX_SIG_LEN 256

SignedUDPEndpoint::SignedUDPEndpoint(const std::string &ip, const int port,
                                     EVP_PKEY *key, const bool isMasterReceiver=false)
    : UDPEndpoint(ip, port, isMasterReceiver), key_(key)
{
}

SignedUDPEndpoint::~SignedUDPEndpoint() {}

int SignedUDPEndpoint::SignAndSendMsgTo(const Address &dstAddr,
                                        const char *msg,
                                        u_int32_t msgLen,
                                        char msgType)
{
    char buffer[sizeof(SignedMessageHeader) + msgLen + MAX_SIG_LEN];
    unsigned char *sig = (unsigned char *)&buffer[sizeof(SignedMessageHeader) + msgLen];
    size_t sigLen = 0;

    SignedMessageHeader *hdr = (SignedMessageHeader *)buffer;

    // TODO Lot of copying here...
    hdr->dataLen = msgLen;
    hdr->sigLen = sigLen;
    memcpy(buffer, msg, msgLen);

    // Write signature after msg
    EVP_MD_CTX *mdctx = NULL;
    if (!(mdctx = EVP_MD_CTX_create()))
        return -1;
    // Use SHA256 as digest to sign
    if (1 != EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, key_))
        return -1;
    if (1 != EVP_DigestSignUpdate(mdctx, msg, strlen(msg)))
        return -1;

    if (1 != EVP_DigestSignFinal(mdctx, sig, &sigLen))
    {
        LOG(ERROR) << "Failed to sign message!\n";
        return -1;
    }

    return SendMsgTo(dstAddr, buffer, sigLen, msgType);
}

int SignedUDPEndpoint::SignAndSendProtoMsg(const Address &dstAddr,
                                           const google::protobuf::Message &msg,
                                           char msgType)
{
    std::string serializedString = msg.SerializeAsString();
    uint32_t msgLen = serializedString.length();
    if (msgLen > 0)
    {
        SignAndSendMsgTo(dstAddr, serializedString.c_str(), msgLen, msgType);
    }
    return -1;
}
