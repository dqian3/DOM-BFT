#include "lib/transport/nng_endpoint.h"
#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"

#include <glog/logging.h>

#include <thread>


int main(int argc, char *argv[])
{
    // Read public key file

    NngEndpoint endpoint({{Address("127.0.0.1", 3000), Address("127.0.0.2", 3001)}});

    MessageHandlerFunc func = [](MessageHeader *hdr, byte *body, Address *sender)
    {
        printf("%d %d %d\n", hdr->msgLen, hdr->msgType, hdr->sigLen);

        uint32_t bodyLen = hdr->msgLen;

        printf("%s\n", body);
 
    };
    
    endpoint.RegisterMsgHandler(func);

    printf("Entering event loop\n");

    endpoint.LoopRun();
    printf("Done loop run!\n");
}