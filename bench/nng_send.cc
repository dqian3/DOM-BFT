#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "proto/dombft_apps.pb.h"
#include "proto/dombft_proto.pb.h"

#include <glog/logging.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

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

    uint32_t client_id = std::stoi(argv[6]);

    std::vector<std::pair<Address, Address>> pairs;
    for (uint32_t i = 0; i < shards; i++) {
        pairs.push_back(
            {Address(srcAddr, srcPort + client_id * shards + i), Address(dstAddr, dstPort + client_id * shards + i)}
        );
    }
    NngEndpoint *endpoint = new NngEndpoint(pairs);

    // Send timer
    for (int i = 0; i < 1000000; i++) {
        // std::string msg = "Test";
        // msg = msg + std::to_string(i);
        // LOG(INFO) << msg;

        // MessageHeader *hdr = endpoint->PrepareMsg((const byte *) msg.c_str(), msg.length(), 2);

        dombft::proto::ClientRequest request;

        dombft::apps::CounterRequest counter_req;
        request.set_client_id(client_id);
        request.set_client_seq(i);

        counter_req.set_op(dombft::apps::CounterOperation::INCREMENT);

        request.set_req_data(counter_req.SerializeAsString());
        MessageHeader *hdr = endpoint->PrepareProtoMsg(request, 2);

        if (i % 10000 == 0) {
            LOG(INFO) << "Sent message " << i;
        }

        endpoint->SendPreparedMsgTo(Address(dstAddr, dstPort + client_id * shards + i % shards), hdr);
    }

    // std::this_thread::sleep_for(std::chrono::seconds(20));
}