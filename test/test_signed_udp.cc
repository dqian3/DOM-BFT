#include "lib/endpoint.h"
#include "lib/signed_udp_endpoint.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

int main(int argc, char *argv[])
{
    // Read private key file
    BIO* bo = BIO_new_file(argv[1], "r");
    EVP_PKEY* key = NULL;
    PEM_read_bio_PrivateKey(bo, &key, 0, 0 );

    if(key == NULL)
        LOG(ERROR) << "Unable to load private key!";
        return 1;
    

    SignedUDPEndpoint udpEndpoint("127.0.0.1", 8000, key);

    // Send timer
    Timer t = Timer([] (void *data, void *endpoint) {
        SignedUDPEndpoint *ep = (SignedUDPEndpoint *) endpoint;
        ep->SendMsgTo(Address("127.0.0.1", 9000), "Test", 5, 2);
    }, 1000);

    udpEndpoint.RegisterTimer(&t);
}