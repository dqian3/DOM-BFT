#include "lib/udp_endpoint.h"
#include "lib/address.h"
#include "lib/message_handler.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <thread>

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

void run(EVP_PKEY *pubkey) 
{
    UDPEndpoint ep("127.0.0.1", 9000);

    MessageHandlerFunc func = [pubkey](MessageHeader *hdr, byte *body, Address *sender, void *context)
    {
        printf("%d %d %d\n", hdr->msgLen, hdr->msgType, hdr->sigLen);

        uint32_t bodyLen = hdr->msgLen;

        // TODO checks
        unsigned char *sig = (unsigned char *)(body + bodyLen);

        if (verify(body, bodyLen, sig, hdr->sigLen, pubkey)) {
            printf("Verified!\n");
            printf("%s\n", body);
        }
        else {
            printf("Failed to verify!\n");
        }

    };
    UDPMessageHandler handler(func, nullptr);
    printf("Registiering message handler!\n");
    ep.RegisterMsgHandler(&handler);

    printf("Entering event loop\n");

    ep.LoopRun();
    printf("Done loop run!\n");

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


    run(pubkey);
}