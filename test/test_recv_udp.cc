#include "lib/udp_endpoint.h"
#include "lib/address.h"
#include "lib/message_handler.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

bool verify(void *body, size_t bodyLen, const unsigned char *sig, size_t sigLen, EVP_PKEY *pubkey)
{
    EVP_MD_CTX *mdctx = NULL;

    /* Create the Message Digest Context */
    if (!(mdctx = EVP_MD_CTX_create())) {
        LOG(ERROR) << "Error creating OpenSSL Context";
        return false;
    }

    if (1 != EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubkey)) {
        LOG(ERROR) << "Error initializing  Context";
        return false;
    }

    /* Initialize `key` with a public key */
    if (1 != EVP_DigestVerifyUpdate(mdctx, body, bodyLen))  {
        LOG(ERROR) << "Error EVP_DigestVerifyUpdate";
        return false;
    }

    if (1 == EVP_DigestVerifyFinal(mdctx, sig, sigLen)) {
        return true;
    }
    else {
        LOG(ERROR) << "signature did not verify :(";
        return false;
    }
}

int main(int argc, char *argv[])
{
    // Read public key file
    BIO *bo = BIO_new_file(argv[1], "r");
    EVP_PKEY *pubkey = NULL;
    PEM_read_bio_PUBKEY(bo, &pubkey, 0, 0);

    if (pubkey == NULL) {
        LOG(ERROR) << "Unable to load public key!";
        return 1;
    }

    UDPEndpoint ep("127.0.0.1", 9000);

    MessageHandlerFunc func = [pubkey](MessageHeader *hdr, void *body, Address *sender, void *context)
    {
        printf("%d %d\n", hdr->msgLen, hdr->msgType);

        SignedMessageHeader *shdr = (SignedMessageHeader *)body;
        printf("%d %d\n", shdr->dataLen, shdr->sigLen);

        // TODO checks
        void *data = body + sizeof(SignedMessageHeader);
        unsigned char *sig = (unsigned char *)(body + sizeof(SignedMessageHeader) + shdr->dataLen);

        if (verify(data, shdr->dataLen, sig, shdr->sigLen, pubkey)) {
            printf("Verified!\n");
            printf("%s\n", data);
        }
        else {
            printf("Failed to verify!\n");
        }

    };
    UDPMsgHandler handler(func, nullptr);
    ep.RegisterMsgHandler(&handler);

    printf("Entering event loop\n");

    ep.LoopRun();
    printf("Done loop run!\n");
}