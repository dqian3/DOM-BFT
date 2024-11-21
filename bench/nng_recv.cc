#include "lib/transport/nng_endpoint.h"

#include "lib/log.h"
#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "proto/dombft_proto.pb.h"

#include <glog/logging.h>

#include <thread>

int main(int argc, char *argv[])
{
    FLAGS_v = 1;
    FLAGS_logtostderr = 1;
    google::InitGoogleLogging(argv[0]);

    std::string srcAddr(argv[1]);
    uint32_t srcPort = std::stoi(argv[2]);
    std::string dstAddr(argv[3]);
    uint32_t dstPort = std::stoi(argv[4]);

    uint32_t shards = std::stoi(argv[5]);

    uint32_t numClients = std::stoi(argv[6]);

    std::vector<std::pair<Address, Address>> pairs;

    for (uint32_t i = 0; i < numClients; i++) {
        for (uint32_t j = 0; j < shards; j++) {
            pairs.push_back({Address(srcAddr, srcPort + i * shards + j), Address(dstAddr, dstPort + i * shards + j)});
        }
    }

    NngEndpointThreaded *endpoint = new NngEndpointThreaded(pairs);

    std::shared_ptr<Log> log_ = std::make_shared<Log>(std::make_shared<Counter>());

    int i = 0;
    bool started = false;
    uint64_t start = 0;

    MessageHandlerFunc func = [&](MessageHeader *hdr, byte *body, Address *sender) {
        dombft::proto::ClientRequest request;

        if (!request.ParseFromArray(body, hdr->msgLen)) {
            LOG(ERROR) << "Unable to parse CLIENT_REQUEST";
            return;
        }

        if (!started && request.client_id() == numClients - 1) {
            start = GetMicrosecondTimestamp();
            started = true;
        }

        if (!started)
            return;

        i++;

        std::string res;

        if (i == 500000) {
            uint64_t recvTime = GetMicrosecondTimestamp() - start;
            LOG(INFO) << "Receiving 500000 requests took " << recvTime << " usec";
            LOG(INFO) << "txput=" << 500000 * 1e+6 / recvTime << " req/s";
            endpoint->LoopBreak();
        }
    };

    endpoint->RegisterMsgHandler(func);
    LOG(INFO) << "Entering event loop\n";

    endpoint->LoopRun();
    LOG(INFO) << "Done loop run!\n";
}