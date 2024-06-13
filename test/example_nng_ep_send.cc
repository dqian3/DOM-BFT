#include "lib/transport/nng_endpoint.h"
#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <thread>

int main()
{
    // Read private key file    
    NngEndpoint endpoint({{Address("127.0.0.2", 3001), Address("127.0.0.1", 3000)}});

    // Send timer
    Timer t = Timer([] (void *data, void *endpoint) {
        LOG(INFO) << "Sending";
        NngEndpoint *ep = (NngEndpoint *) endpoint;
        
        MessageHeader *hdr = ep->PrepareMsg((const byte *)"Test", 5, 2);
        LOG(INFO) << hdr->msgLen << " " << hdr->sigLen;
        ep->SendPreparedMsgTo(Address("127.0.0.1", 3000));
    }, 1000000);

    endpoint.RegisterTimer(&t);
    endpoint.LoopRun();

}