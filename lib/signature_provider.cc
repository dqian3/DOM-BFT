#include "lib/signature_provider.h"
#include <glog/logging.h>

#define MAX_SIG_LEN 256

SignatureProvider::SignatureProvider(EVP_PKEY *privateKey)
    : privKey_(privateKey)
{
}

SignatureProvider::~SignatureProvider() {}

int SignatureProvider::appendSignature(MessageHeader *hdr, uint32_t bufLen)
{
    size_t sigLen;

    if (hdr->msgLen + sizeof(MessageHeader) > bufLen)
    {
        LOG(ERROR) << "Error signing message, inital message size "
                   << hdr->msgLen + sizeof(MessageHeader)
                   << " exceeds given buffer capacity "
                   << bufLen;

        return -1;
    }

    byte *data = (byte *)(hdr + 1);
    byte *sig = data + hdr->msgLen;

    // Write signature after msg
    EVP_MD_CTX *mdctx = NULL;
    if (!(mdctx = EVP_MD_CTX_create()))
        return -1;
    // Use SHA256 as digest to sign
    if (1 != EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, privKey_))
        return -1;
    if (1 != EVP_DigestSignUpdate(mdctx, data, hdr->msgLen))
        return -1;

    if (1 != EVP_DigestSignFinal(mdctx, NULL, &sigLen))
    {
        LOG(ERROR) << "Failed to calculate signature length!\n";
        return -1;
    }
    hdr->sigLen = sigLen;

    if (hdr->msgLen + hdr->sigLen + sizeof(MessageHeader) > bufLen)
    {
        LOG(ERROR) << "Error signing message, not enough room in buffer for signature!";
        return -1;
    }

    if (1 != EVP_DigestSignFinal(mdctx, sig, &sigLen))
    {
        LOG(ERROR) << "Failed to sign message!\n";
        return -1;
    }

    return 0;
}

bool SignatureProvider::verify(MessageHeader *hdr, byte *body, EVP_PKEY *pubkey)
{
    EVP_MD_CTX *mdctx = NULL;

    byte *data = (byte *)(hdr + 1);

    /* Create the Message Digest Context */
    if (!(mdctx = EVP_MD_CTX_create()))
    {
        LOG(ERROR) << "Error creating OpenSSL Context";
        return false;
    }

    if (1 != EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubkey))
    {
        LOG(ERROR) << "Error initializing digest context";
        return false;
    }

    /* Initialize `key` with a public key */
    if (1 != EVP_DigestVerifyUpdate(mdctx, data, hdr->msgLen))
    {
        LOG(ERROR) << "Error EVP_DigestVerifyUpdate";
        return false;
    }

    if (1 == EVP_DigestVerifyFinal(mdctx, data + hdr->msgLen, hdr->sigLen))
    {
        return true;
    }
    else
    {
        LOG(ERROR) << "signature did not verify :(";
        return false;
    }
}