#include "lib/transport/nng_endpoint.h"

#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"
#include "lib/transport/nng_endpoint_threaded.h"

#include <glog/logging.h>

#include <thread>

int main(int argc, char *argv[])
{
    FLAGS_v = 10;
    FLAGS_logtostderr = 1;
    google::InitGoogleLogging(argv[0]);

    NngEndpointThreaded endpoint({{Address("127.0.0.1", 3000), Address("127.0.0.2", 3001)}});

    MessageHandlerFunc func = [](MessageHeader *hdr, byte *body, Address *sender) {
        LOG(INFO) << hdr->msgLen << " " << hdr->msgType << " " << hdr->sigLen;
        LOG(INFO) << body;
    };

    endpoint.RegisterMsgHandler(func);

    LOG(INFO) << "Entering event loop\n";

    endpoint.LoopRun();
    LOG(INFO) << "Done loop run!\n";
}