#include "lib/transport/endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "lib/signature_provider.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

int main(int argc, char *argv[])
{
    // Read private key file    
    UDPEndpoint udpEndpoint("127.0.0.1", 8000);
    SignatureProvider sigProvider;
    sigProvider.loadPrivateKey(argv[1]);

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