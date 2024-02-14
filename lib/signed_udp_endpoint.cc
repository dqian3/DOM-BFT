#include "lib/signed_udp_endpoint.h"

#define MAX_SIG_LEN 256

SignedUDPEndpoint::SignedUDPEndpoint(const std::string &ip, const int port,
                                     EVP_PKEY *key, const bool isMasterReceiver)
    : UDPEndpoint(ip, port, isMasterReceiver), key_(key)
{
}

SignedUDPEndpoint::~SignedUDPEndpoint() {}

int SignedUDPEndpoint::SignAndSendMsgTo(const Address &dstAddr,
                                        const char *msg,
                                        u_int32_t msgLen,
                                        char msgType)
{
    char buffer[sizeof(MessageHeader) + sizeof(SignedMessageHeader) + msgLen + MAX_SIG_LEN];
    size_t sigLen = 0;

    MessageHeader *hdr = (MessageHeader *) buffer;
    SignedMessageHeader *shdr = (SignedMessageHeader *)(hdr + 1);
    unsigned char *data = (unsigned char *)(shdr + 1);
    unsigned char *sig = data + msgLen;

    memcpy(data, msg, msgLen);

    // Write signature after msg
    EVP_MD_CTX *mdctx = NULL;
    if (!(mdctx = EVP_MD_CTX_create()))
        return -1;
    // Use SHA256 as digest to sign
    if (1 != EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, key_))
        return -1;
    if (1 != EVP_DigestSignUpdate(mdctx, data, msgLen))
        return -1;

    if(1 != EVP_DigestSignFinal(mdctx, NULL, &sigLen)) {
        LOG(ERROR) << "Failed to calculate signature length!\n";
        return -1;
    }
    shdr->sigLen = sigLen;
    hdr->msgLen = sizeof(SignedMessageHeader) + msgLen + sigLen;
    hdr->msgType = msgType;

    if (1 != EVP_DigestSignFinal(mdctx, sig, &sigLen))
    {
        LOG(ERROR) << "Failed to sign message!\n";
        return -1;
    }

    VLOG(3) << "Sending to " << dstAddr.ip_ << ", " << dstAddr.port_;

    int ret = sendto(fd_, buffer, hdr->msgLen + sizeof(MessageHeader), 0,
                     (struct sockaddr *)(&(dstAddr.addr_)), sizeof(sockaddr_in));
    if (ret < 0)
    {
        VLOG(1) << "Send Fail ret =" << ret << ". Error: " << strerror(errno);
    }
    return ret;

}

int SignedUDPEndpoint::SignAndSendProtoMsgTo(const Address &dstAddr,
                                           const google::protobuf::Message &msg,
                                           char msgType)
{
    std::string serializedString = msg.SerializeAsString();
    uint32_t msgLen = serializedString.length();

    VLOG(3) << "Serializing protobuf message type " << (int) msgType << " with len " << msgLen;

    if (msgLen > 0)
    {
        SignAndSendMsgTo(dstAddr, serializedString.c_str(), msgLen, msgType);
    }
    return -1;
}
