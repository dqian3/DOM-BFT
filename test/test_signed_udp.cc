#include "lib/endpoint.h"
#include "lib/udp_endpoint.h"
#include "lib/signature_provider.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

int main(int argc, char *argv[])
{
    // Read private key file
    BIO* bo = BIO_new_file(argv[1], "r");
    EVP_PKEY* key = NULL;
    PEM_read_bio_PrivateKey(bo, &key, 0, 0 );

    if(key == NULL) {
        LOG(ERROR) << "Unable to load private key!";
        return 1;
    }
    

    UDPEndpoint udpEndpoint("127.0.0.1", 8000);
    SignatureProvider sigProvider(key);

    // Send timer
    Timer t = Timer([&sigProvider] (void *data, void *endpoint) {
        LOG(INFO) << "Sending";
        UDPEndpoint *ep = (UDPEndpoint *) endpoint;
        
        MessageHeader *hdr = ep->PrepareMsg((const byte *)"Test", 5, 2);
        sigProvider.appendSignature(hdr, UDP_BUFFER_SIZE); // TODO more elegant way to get UDPBufferSize
        LOG(INFO) << hdr->msgLen << " " << hdr->sigLen;
        ep->SendPreparedMsgTo(Address("127.0.0.1", 9000));
    }, 1000000);

    udpEndpoint.RegisterTimer(&t);
    udpEndpoint.LoopRun();

}